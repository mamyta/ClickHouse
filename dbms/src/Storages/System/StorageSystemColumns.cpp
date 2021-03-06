#include <DB/Storages/System/StorageSystemColumns.h>
#include <DB/Storages/MergeTree/MergeTreeData.h>
#include <DB/Storages/StorageMergeTree.h>
#include <DB/Storages/StorageReplicatedMergeTree.h>
#include <DB/Columns/ColumnString.h>
#include <DB/DataTypes/DataTypeString.h>
#include <DB/DataTypes/DataTypesNumberFixed.h>
#include <DB/DataStreams/OneBlockInputStream.h>
#include <DB/Common/VirtualColumnUtils.h>
#include <DB/Parsers/queryToString.h>
#include <DB/Databases/IDatabase.h>


namespace DB
{

StorageSystemColumns::StorageSystemColumns(const std::string & name_)
	: name(name_)
	, columns{
		{ "database",           std::make_shared<DataTypeString>() },
		{ "table",              std::make_shared<DataTypeString>() },
		{ "name",               std::make_shared<DataTypeString>() },
		{ "type",               std::make_shared<DataTypeString>() },
		{ "default_type",       std::make_shared<DataTypeString>() },
		{ "default_expression", std::make_shared<DataTypeString>() },
		{ "bytes",				std::make_shared<DataTypeUInt64>() },
	}
{
}

StoragePtr StorageSystemColumns::create(const std::string & name_)
{
	return make_shared(name_);
}

BlockInputStreams StorageSystemColumns::read(
	const Names & column_names,
	ASTPtr query,
	const Context & context,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	const size_t max_block_size,
	const unsigned threads)
{
	check(column_names);
	processed_stage = QueryProcessingStage::FetchColumns;

	Block block;

	std::map<std::pair<std::string, std::string>, StoragePtr> storages;

	{
		Databases databases = context.getDatabases();

		/// Добавляем столбец database.
		ColumnPtr database_column = std::make_shared<ColumnString>();
		for (const auto & database : databases)
			database_column->insert(database.first);
		block.insert(ColumnWithTypeAndName(database_column, std::make_shared<DataTypeString>(), "database"));

		/// Отфильтруем блок со столбцом database.
		VirtualColumnUtils::filterBlockWithQuery(query, block, context);

		if (!block.rows())
			return BlockInputStreams();

		database_column = block.getByName("database").column;
		size_t rows = database_column->size();

		/// Добавляем столбец table.
		ColumnPtr table_column = std::make_shared<ColumnString>();
		IColumn::Offsets_t offsets(rows);
		for (size_t i = 0; i < rows; ++i)
		{
			const std::string database_name = (*database_column)[i].get<std::string>();
			const DatabasePtr database = databases.at(database_name);
			offsets[i] = i ? offsets[i - 1] : 0;

			for (auto iterator = database->getIterator(); iterator->isValid(); iterator->next())
			{
				const String & table_name = iterator->name();
				storages.emplace(std::piecewise_construct,
					std::forward_as_tuple(database_name, table_name),
					std::forward_as_tuple(iterator->table()));
				table_column->insert(table_name);
				offsets[i] += 1;
			}
		}

		for (size_t i = 0; i < block.columns(); ++i)
		{
			ColumnPtr & column = block.safeGetByPosition(i).column;
			column = column->replicate(offsets);
		}

		block.insert(ColumnWithTypeAndName(table_column, std::make_shared<DataTypeString>(), "table"));
	}

	/// Отфильтруем блок со столбцами database и table.
	VirtualColumnUtils::filterBlockWithQuery(query, block, context);

	if (!block.rows())
		return BlockInputStreams();

	ColumnPtr filtered_database_column = block.getByName("database").column;
	ColumnPtr filtered_table_column = block.getByName("table").column;

	/// Составляем результат.
	ColumnPtr database_column = std::make_shared<ColumnString>();
	ColumnPtr table_column = std::make_shared<ColumnString>();
	ColumnPtr name_column = std::make_shared<ColumnString>();
	ColumnPtr type_column = std::make_shared<ColumnString>();
	ColumnPtr default_type_column = std::make_shared<ColumnString>();
	ColumnPtr default_expression_column = std::make_shared<ColumnString>();
	ColumnPtr bytes_column = std::make_shared<ColumnUInt64>();

	size_t rows = filtered_database_column->size();
	for (size_t i = 0; i < rows; ++i)
	{
		const std::string database_name = (*filtered_database_column)[i].get<std::string>();
		const std::string table_name = (*filtered_table_column)[i].get<std::string>();

		NamesAndTypesList columns;
		ColumnDefaults column_defaults;
		std::unordered_map<String, size_t> column_sizes;

		{
			StoragePtr storage = storages.at(std::make_pair(database_name, table_name));
			TableStructureReadLockPtr table_lock;

			try
			{
				table_lock = storage->lockStructure(false);
			}
			catch (const Exception & e)
			{
				/** There are case when IStorage::drop was called,
				  *  but we still own the object.
				  * Then table will throw exception at attempt to lock it.
				  * Just skip the table.
				  */
				if (e.code() == ErrorCodes::TABLE_IS_DROPPED)
					continue;
				else
					throw;
			}

			columns = storage->getColumnsList();
			columns.insert(std::end(columns), std::begin(storage->alias_columns), std::end(storage->alias_columns));
			column_defaults = storage->column_defaults;

			/** Данные о размерах столбцов для таблиц семейства MergeTree.
			  * NOTE: В дальнейшем можно сделать интерфейс, позволяющий получить размеры столбцов у IStorage.
			  */
			if (auto storage_concrete = dynamic_cast<StorageMergeTree *>(storage.get()))
			{
				column_sizes = storage_concrete->getData().getColumnSizes();
			}
			else if (auto storage_concrete = dynamic_cast<StorageReplicatedMergeTree *>(storage.get()))
			{
				column_sizes = storage_concrete->getData().getColumnSizes();

				auto unreplicated_data = storage_concrete->getUnreplicatedData();
				if (unreplicated_data)
				{
					auto unreplicated_column_sizes = unreplicated_data->getColumnSizes();
					for (const auto & name_size : unreplicated_column_sizes)
						column_sizes[name_size.first] += name_size.second;
				}
			}
		}

		for (const auto & column : columns)
		{
			database_column->insert(database_name);
			table_column->insert(table_name);
			name_column->insert(column.name);
			type_column->insert(column.type->getName());

			{
				const auto it = column_defaults.find(column.name);
				if (it == std::end(column_defaults))
				{
					default_type_column->insertDefault();
					default_expression_column->insertDefault();
				}
				else
				{
					default_type_column->insert(toString(it->second.type));
					default_expression_column->insert(queryToString(it->second.expression));
				}
			}

			{
				const auto it = column_sizes.find(column.name);
				if (it == std::end(column_sizes))
					bytes_column->insertDefault();
				else
					bytes_column->insert(it->second);
			}
		}
	}

	block.clear();

	block.insert(ColumnWithTypeAndName(database_column, std::make_shared<DataTypeString>(), "database"));
	block.insert(ColumnWithTypeAndName(table_column, std::make_shared<DataTypeString>(), "table"));
	block.insert(ColumnWithTypeAndName(name_column, std::make_shared<DataTypeString>(), "name"));
	block.insert(ColumnWithTypeAndName(type_column, std::make_shared<DataTypeString>(), "type"));
	block.insert(ColumnWithTypeAndName(default_type_column, std::make_shared<DataTypeString>(), "default_type"));
	block.insert(ColumnWithTypeAndName(default_expression_column, std::make_shared<DataTypeString>(), "default_expression"));
	block.insert(ColumnWithTypeAndName(bytes_column, std::make_shared<DataTypeUInt64>(), "bytes"));

	return BlockInputStreams{ 1, std::make_shared<OneBlockInputStream>(block) };
}

}
