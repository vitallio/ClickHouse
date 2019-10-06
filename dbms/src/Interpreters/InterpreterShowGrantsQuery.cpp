#include <Interpreters/InterpreterShowGrantsQuery.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTGrantQuery.h>
#include <Parsers/ASTShowGrantsQuery.h>
#include <Parsers/formatAST.h>
#include <Access/AccessControlManager.h>
#include <Access/Role.h>
#include <Columns/ColumnString.h>
#include <DataStreams/OneBlockInputStream.h>
#include <DataTypes/DataTypeString.h>
#include <common/StringRef.h>
#include <map>
#include <sstream>


namespace DB
{
BlockIO InterpreterShowGrantsQuery::execute()
{
    BlockIO res;
    res.in = executeImpl();
    return res;
}


BlockInputStreamPtr InterpreterShowGrantsQuery::executeImpl()
{
    auto grant_queries = enumerateGrantQueries();

    MutableColumnPtr column = ColumnString::create();
    for (const auto & grant_query : grant_queries)
    {
        std::stringstream stream;
        formatAST(*grant_query, stream, false, true);
        column->insert(stream.str());
    }

    const auto & query = query_ptr->as<ASTShowGrantsQuery &>();
    return std::make_shared<OneBlockInputStream>(Block{{
        std::move(column),
        std::make_shared<DataTypeString>(),
        "Grants for " + query.role_name}});
}


std::vector<ASTPtr> InterpreterShowGrantsQuery::enumerateGrantQueries() const
{
    const auto & query = query_ptr->as<ASTShowGrantsQuery &>();
    auto & manager = context.getAccessControlManager();
    const String & current_database = context.getCurrentDatabase();

    auto role = manager.read<Role>(query.role_name);
    std::vector<ASTPtr> result;
    using Kind = ASTGrantQuery::Kind;

    for (bool grant_option : {false, true})
    {
        std::map<std::pair<StringRef /* database */, StringRef /* table */>, std::shared_ptr<ASTGrantQuery>> grants;
        std::map<std::pair<StringRef /* database */, StringRef /* table */>, std::shared_ptr<ASTGrantQuery>> partial_revokes;

        for (const auto & priv : role->privileges[grant_option].getInfo())
        {
            for (Kind kind : {Kind::GRANT, Kind::REVOKE})
            {
                auto access = (kind == Kind::GRANT) ? priv.grants : priv.partial_revokes;
                if (!access)
                    continue;

                auto & map = (kind == Kind::GRANT) ? grants : partial_revokes;
                auto it = map.find({priv.database, priv.table});
                if (it == map.end())
                {
                    auto new_grant_query = std::make_shared<ASTGrantQuery>();
                    new_grant_query->to_roles.emplace_back(role->name);
                    if (priv.database == current_database)
                        new_grant_query->use_current_database = true;
                    else
                        new_grant_query->database = priv.database;
                    new_grant_query->table = priv.table;
                    new_grant_query->kind = kind;
                    new_grant_query->grant_option = grant_option;
                    it = map.try_emplace({new_grant_query->database, new_grant_query->table}, new_grant_query).first;
                }

                auto & grant_query = *(it->second);
                if (priv.column.empty())
                    grant_query.access |= access;
                else
                    grant_query.columns_access[priv.column] |= access;
            }
        }

        for (Kind kind : {Kind::GRANT, Kind::REVOKE})
        {
            const auto & grant_map = (kind == Kind::GRANT) ? grants : partial_revokes;
            for (const auto & key_and_grant_query : grant_map)
            {
                const auto grant_query = key_and_grant_query.second;
                result.push_back(grant_query);
            }
        }
    }

    for (bool admin_option : {false, true})
    {
        Strings granted_roles;
        for (const UUID & granted_role_id : role->granted_roles[admin_option])
        {
            auto granted_role_name = manager.tryReadName(granted_role_id);
            if (granted_role_name)
                granted_roles.emplace_back(*granted_role_name);
        }

        if (!granted_roles.empty())
        {
            std::sort(granted_roles.begin(), granted_roles.end());
            auto new_query = std::make_shared<ASTGrantQuery>();
            new_query->to_roles.emplace_back(role->name);
            new_query->kind = Kind::GRANT;
            new_query->grant_option = admin_option;
            new_query->roles = std::move(granted_roles);
            result.push_back(std::move(new_query));
        }
    }
    return result;
}
}
