#include <iomanip>

#include <Poco/Net/HTTPBasicCredentials.h>

#include <DB/Common/Stopwatch.h>
#include <DB/Common/StringUtils.h>

#include <DB/IO/ReadBufferFromIStream.h>
#include <DB/IO/ZlibInflatingReadBuffer.h>
#include <DB/IO/ReadBufferFromString.h>
#include <DB/IO/ConcatReadBuffer.h>
#include <DB/IO/CompressedReadBuffer.h>
#include <DB/IO/CompressedWriteBuffer.h>
#include <DB/IO/WriteBufferFromString.h>
#include <DB/IO/WriteHelpers.h>

#include <DB/DataStreams/IProfilingBlockInputStream.h>

#include <DB/Interpreters/executeQuery.h>
#include <DB/Interpreters/Quota.h>

#include <DB/Common/ExternalTable.h>

#include "HTTPHandler.h"



namespace DB
{

namespace ErrorCodes
{
	extern const int READONLY;
	extern const int UNKNOWN_COMPRESSION_METHOD;
}


void HTTPHandler::processQuery(
	Poco::Net::HTTPServerRequest & request,
	HTMLForm & params,
	Poco::Net::HTTPServerResponse & response,
	Output & used_output)
{
	LOG_TRACE(log, "Request URI: " << request.getURI());

	std::istream & istr = request.stream();

	/// Part of the query can be passed in the 'query' parameter and the rest in the request body
	/// (http method need not necessarily be POST). In this case the entire query consists of the
	/// contents of the 'query' parameter, a line break and the request body.
	std::string query_param = params.get("query", "");
	if (!query_param.empty())
		query_param += '\n';


	/// The client can pass a HTTP header indicating supported compression method (gzip or deflate).
	String http_response_compression_methods = request.get("Accept-Encoding", "");
	bool client_supports_http_compression = false;
	ZlibCompressionMethod http_response_compression_method {};

	if (!http_response_compression_methods.empty())
	{
		/// Both gzip and deflate are supported. If the client supports both, gzip is preferred.
		/// NOTE parsing of the list of methods is slightly incorrect.
		if (std::string::npos != http_response_compression_methods.find("gzip"))
		{
			client_supports_http_compression = true;
			http_response_compression_method = ZlibCompressionMethod::Gzip;
		}
		else if (std::string::npos != http_response_compression_methods.find("deflate"))
		{
			client_supports_http_compression = true;
			http_response_compression_method = ZlibCompressionMethod::Zlib;
		}
	}

	used_output.out = std::make_shared<WriteBufferFromHTTPServerResponse>(
		response, client_supports_http_compression, http_response_compression_method);

	/// Client can pass a 'compress' flag in the query string. In this case the query result is
	/// compressed using internal algorithm. This is not reflected in HTTP headers.
	if (parse<bool>(params.get("compress", "0")))
		used_output.out_maybe_compressed = std::make_shared<CompressedWriteBuffer>(*used_output.out);
	else
		used_output.out_maybe_compressed = used_output.out;

	/// User name and password can be passed using query parameters or using HTTP Basic auth (both methods are insecure).
	/// The user and password can be passed by headers (similar to X-Auth-*), which is used by load balancers to pass authentication information
	std::string user = request.get("X-ClickHouse-User", params.get("user", "default"));
	std::string password = request.get("X-ClickHouse-Key", params.get("password", ""));

	if (request.hasCredentials())
	{
		Poco::Net::HTTPBasicCredentials credentials(request);

		user = credentials.getUsername();
		password = credentials.getPassword();
	}

	std::string quota_key = request.get("X-ClickHouse-Quota", params.get("quota_key", ""));
	std::string query_id = params.get("query_id", "");

	Context context = *server.global_context;
	context.setGlobalContext(*server.global_context);

	context.setUser(user, password, request.clientAddress(), quota_key);
	context.setCurrentQueryId(query_id);

	std::unique_ptr<ReadBuffer> in_param = std::make_unique<ReadBufferFromString>(query_param);

	std::unique_ptr<ReadBuffer> in_post_raw = std::make_unique<ReadBufferFromIStream>(istr);

	/// Request body can be compressed using algorithm specified in the Content-Encoding header.
	std::unique_ptr<ReadBuffer> in_post;
	String http_request_compression_method_str = request.get("Content-Encoding", "");
	if (!http_request_compression_method_str.empty())
	{
		ZlibCompressionMethod method;
		if (http_request_compression_method_str == "gzip")
		{
			method = ZlibCompressionMethod::Gzip;
		}
		else if (http_request_compression_method_str == "deflate")
		{
			method = ZlibCompressionMethod::Zlib;
		}
		else
			throw Exception("Unknown Content-Encoding of HTTP request: " + http_request_compression_method_str,
				ErrorCodes::UNKNOWN_COMPRESSION_METHOD);
		in_post = std::make_unique<ZlibInflatingReadBuffer>(*in_post_raw, method);
	}
	else
		in_post = std::move(in_post_raw);

	/// The data can also be compressed using incompatible internal algorithm. This is indicated by
	/// 'decompress' query parameter.
	std::unique_ptr<ReadBuffer> in_post_maybe_compressed;
	bool in_post_compressed = false;
	if (parse<bool>(params.get("decompress", "0")))
	{
		in_post_maybe_compressed = std::make_unique<CompressedReadBuffer>(*in_post);
		in_post_compressed = true;
	}
	else
		in_post_maybe_compressed = std::move(in_post);

	std::unique_ptr<ReadBuffer> in;

	/// Support for "external data for query processing".
	if (startsWith(request.getContentType().data(), "multipart/form-data"))
	{
		in = std::move(in_param);
		ExternalTablesHandler handler(context, params);

		params.load(request, istr, handler);

		/// Erase unneeded parameters to avoid confusing them later with context settings or query
		/// parameters.
		for (const auto & it : handler.names)
		{
			params.erase(it + "_format");
			params.erase(it + "_types");
			params.erase(it + "_structure");
		}
	}
	else
		in = std::make_unique<ConcatReadBuffer>(*in_param, *in_post_maybe_compressed);

	/// Settings can be overridden in the query.
	/// Some parameters (database, default_format, everything used in the code above) do not
	/// belong to the Settings class.

	/// 'readonly' setting values mean:
	/// readonly = 0 - any query is allowed, client can change any setting.
	/// readonly = 1 - only readonly queries are allowed, client can't change settings.
	/// readonly = 2 - only readonly queries are allowed, client can change any setting except 'readonly'.

	/// In theory if initially readonly = 0, the client can change any setting and then set readonly
	/// to some other value.
	auto & limits = context.getSettingsRef().limits;

	/// Only readonly queries are allowed for HTTP GET requests.
	if (request.getMethod() == Poco::Net::HTTPServerRequest::HTTP_GET)
	{
		if (limits.readonly == 0)
			limits.readonly = 2;
	}

	auto readonly_before_query = limits.readonly;

	for (Poco::Net::NameValueCollection::ConstIterator it = params.begin(); it != params.end(); ++it)
	{
		if (it->first == "database")
		{
			context.setCurrentDatabase(it->second);
		}
		else if (it->first == "default_format")
		{
			context.setDefaultFormat(it->second);
		}
		else if (it->first == "query"
			|| it->first == "compress"
			|| it->first == "decompress"
			|| it->first == "user"
			|| it->first == "password"
			|| it->first == "quota_key"
			|| it->first == "query_id"
			|| it->first == "stacktrace")
		{
		}
		else
		{
			/// All other query parameters are treated as settings.

			if (readonly_before_query == 1)
				throw Exception("Cannot override setting (" + it->first + ") in readonly mode", ErrorCodes::READONLY);

			if (readonly_before_query && it->first == "readonly")
				throw Exception("Setting 'readonly' cannot be overrided in readonly mode", ErrorCodes::READONLY);

			context.setSetting(it->first, it->second);
		}
	}

	/// HTTP response compression is turned on only if the client signalled that they support it
	/// (using Accept-Encoding header) and 'enable_http_compression' setting is turned on.
	used_output.out->setCompression(client_supports_http_compression && context.getSettingsRef().enable_http_compression);
	if (client_supports_http_compression)
		used_output.out->setCompressionLevel(context.getSettingsRef().http_zlib_compression_level);

	/// If 'http_native_compression_disable_checksumming_on_decompress' setting is turned on,
	/// checksums of client data compressed with internal algorithm are not checked.
	if (in_post_compressed && context.getSettingsRef().http_native_compression_disable_checksumming_on_decompress)
		static_cast<CompressedReadBuffer &>(*in_post_maybe_compressed).disableChecksumming();

	/// Add CORS header if 'add_http_cors_header' setting is turned on and the client passed
	/// Origin header.
	used_output.out->addHeaderCORS( context.getSettingsRef().add_http_cors_header && !request.get("Origin", "").empty() );

	ClientInfo & client_info = context.getClientInfo();
	client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
	client_info.interface = ClientInfo::Interface::HTTP;

	/// Query sent through HTTP interface is initial.
	client_info.initial_user = client_info.current_user;
	client_info.initial_query_id = client_info.current_query_id;
	client_info.initial_address = client_info.current_address;

	ClientInfo::HTTPMethod http_method = ClientInfo::HTTPMethod::UNKNOWN;
	if (request.getMethod() == Poco::Net::HTTPServerRequest::HTTP_GET)
		http_method = ClientInfo::HTTPMethod::GET;
	else if (request.getMethod() == Poco::Net::HTTPServerRequest::HTTP_POST)
		http_method = ClientInfo::HTTPMethod::POST;

	client_info.http_method = http_method;
	client_info.http_user_agent = request.get("User-Agent", "");

	executeQuery(*in, *used_output.out_maybe_compressed, /* allow_into_outfile = */ false, context,
		[&response] (const String & content_type) { response.setContentType(content_type); });

	/// Send HTTP headers with code 200 if no exception happened and the data is still not sent to
	/// the client.
	used_output.out->finalize();
}


void HTTPHandler::trySendExceptionToClient(const std::string & s,
	Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response,
	Output & used_output)
{
	try
	{
		/// If HTTP method is POST and Keep-Alive is turned on, we should read the whole request body
		/// to avoid reading part of the current request body in the next request.
		if (request.getMethod() == Poco::Net::HTTPRequest::HTTP_POST
			&& response.getKeepAlive()
			&& !request.stream().eof())
		{
			request.stream().ignore(std::numeric_limits<std::streamsize>::max());
		}

		response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);

		if (!response.sent() && !used_output.out_maybe_compressed)
		{
			/// If nothing was sent yet and we don't even know if we must compress the response.
			response.send() << s << std::endl;
		}
		else if (used_output.out_maybe_compressed)
		{
			/// Send the error message into already used (and possibly compressed) stream.
			/// Note that the error message will possibly be sent after some data.
			/// Also HTTP code 200 could have already been sent.

			/** If buffer has data, and that data wasn't sent yet, then no need to send that data */
			if (used_output.out->count() - used_output.out->offset() == 0)
			{
				used_output.out_maybe_compressed->position() = used_output.out_maybe_compressed->buffer().begin();
				used_output.out->position() = used_output.out->buffer().begin();
			}

			writeString(s, *used_output.out_maybe_compressed);
			writeChar('\n', *used_output.out_maybe_compressed);
			used_output.out_maybe_compressed->next();
			used_output.out->finalize();
		}
	}
	catch (...)
	{
		LOG_ERROR(log, "Cannot send exception to client");
	}
}


void HTTPHandler::handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
{
	Output used_output;

	/// In case of exception, send stack trace to client.
	bool with_stacktrace = false;

	try
	{
		response.setContentType("text/plain; charset=UTF-8");

		/// For keep-alive to work.
		if (request.getVersion() == Poco::Net::HTTPServerRequest::HTTP_1_1)
			response.setChunkedTransferEncoding(true);

		HTMLForm params(request);
		with_stacktrace = parse<bool>(params.get("stacktrace", "0"));

		processQuery(request, params, response, used_output);
		LOG_INFO(log, "Done processing query");
	}
	catch (...)
	{
		tryLogCurrentException(log);

		std::string exception_message = getCurrentExceptionMessage(with_stacktrace);

		/** If exception is received from remote server, then stack trace is embedded in message.
		  * If exception is thrown on local server, then stack trace is in separate field.
		  */

		auto embedded_stack_trace_pos = exception_message.find("Stack trace");
		if (std::string::npos != embedded_stack_trace_pos && !with_stacktrace)
			exception_message.resize(embedded_stack_trace_pos);

		trySendExceptionToClient(exception_message, request, response, used_output);
	}
}


}
