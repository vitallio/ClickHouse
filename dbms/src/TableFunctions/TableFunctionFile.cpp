#include <Storages/StorageFile.h>
#include <Storages/ColumnsDescription.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/TableFunctionFile.h>
#include "registerTableFunctions.h"

namespace DB
{
StoragePtr TableFunctionFile::getStorage(
    const String & source, const String & format, const ColumnsDescription & columns, Context & global_context, const std::string & table_name, const std::string & compression_method) const
{
    StorageFile::CommonArguments args{getDatabaseName(), table_name, format, compression_method, columns, ConstraintsDescription{}, global_context};

    return StorageFile::create(source, global_context.getUserFilesPath(), args);
}

AccessType TableFunctionFile::getRequiredAccessType() const
{
    return AccessType::file;
}

void registerTableFunctionFile(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionFile>();
}
}
