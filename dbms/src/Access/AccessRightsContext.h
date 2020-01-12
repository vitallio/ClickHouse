#pragma once

#include <Access/AccessRights.h>


namespace DB
{
class AccessRightsContext
{
public:
    /// Default constructor creates access rights' context which allows everything.
    AccessRightsContext();

    AccessRightsContext(const String & user_name_, const AccessRights & granted_to_user, bool readonly, bool allow_ddl);

    /// Checks if a specified access granted, and throws an exception if not.
    void check(const AccessType & access) const;
    void check(const AccessType & access, const std::string_view & database) const;
    void check(const AccessType & access, const Strings & databases) const;
    void check(const AccessType & access, const std::string_view & database, const std::string_view & table_or_dictionary) const;
    void check(const AccessType & access, const std::string_view & database, const Strings & tables_or_dictionaries) const;
    void check(const AccessType & access, const std::string_view & database, const std::string_view & table_or_dictionary, const std::string_view & column_or_attribute) const;
    void check(const AccessType & access, const std::string_view & database, const std::string_view & table_or_dictionary, const Strings & columns_or_attributes) const;

private:
    template <typename... Args>
    void checkImpl(const AccessType & access, const Args &... args) const;

    String user_name;
    AccessRights granted_to_user;
    bool readonly;
    bool allow_ddl;
    AccessRights result_access;
};

}
