﻿#pragma once

#include <Access/AccessRights.h>
#include <Access/RowPolicy.h>
#include <Interpreters/ClientInfo.h>
#include <Core/UUID.h>
#include <ext/scope_guard.h>
#include <ext/shared_ptr_helper.h>
#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/container/flat_set.hpp>
#include <mutex>


namespace Poco { class Logger; }

namespace DB
{
struct User;
using UserPtr = std::shared_ptr<const User>;
struct EnabledRolesInfo;
class EnabledRoles;
class EnabledRowPolicies;
class EnabledQuota;
struct Settings;
class SettingsConstraints;
class SettingsProfilesWatcher;
class AccessControlManager;
class IAST;
using ASTPtr = std::shared_ptr<IAST>;


class ContextAccess
{
public:
    struct Params
    {
        std::optional<UUID> user_id;
        std::vector<UUID> current_roles;
        bool use_default_roles = false;
        UInt64 readonly = 0;
        bool allow_ddl = false;
        bool allow_introspection = false;
        String current_database;
        ClientInfo::Interface interface = ClientInfo::Interface::TCP;
        ClientInfo::HTTPMethod http_method = ClientInfo::HTTPMethod::UNKNOWN;
        Poco::Net::IPAddress address;
        String quota_key;
        String default_profile_name;

        friend bool operator ==(const Params & lhs, const Params & rhs);
        friend bool operator !=(const Params & lhs, const Params & rhs) { return !(lhs == rhs); }
        friend bool operator <(const Params & lhs, const Params & rhs);
        friend bool operator >(const Params & lhs, const Params & rhs) { return rhs < lhs; }
        friend bool operator <=(const Params & lhs, const Params & rhs) { return !(rhs < lhs); }
        friend bool operator >=(const Params & lhs, const Params & rhs) { return !(lhs < rhs); }
    };

    /// Default constructor creates access rights' context which allows everything.
    ContextAccess();

    const Params & getParams() const { return params; }
    UserPtr getUser() const;
    String getUserName() const;

    void checkPassword(const String & password) const;
    void checkHostIsAllowed() const;

    std::shared_ptr<const EnabledRolesInfo> getRolesInfo() const;
    std::vector<UUID> getCurrentRoles() const;
    Strings getCurrentRolesNames() const;
    std::vector<UUID> getEnabledRoles() const;
    Strings getEnabledRolesNames() const;

<<<<<<< HEAD:dbms/src/Access/ContextAccess.h
    std::shared_ptr<const EnabledRowPolicies> getRowPolicies() const;
    ASTPtr getRowPolicyCondition(const String & database, const String & table_name, RowPolicy::ConditionType index, const ASTPtr & extra_condition = nullptr) const;
    std::shared_ptr<const EnabledQuota> getQuota() const;
=======
    RowPolicyContextPtr getRowPolicy() const;
    QuotaContextPtr getQuota() const;
    std::shared_ptr<const Settings> getDefaultSettings() const;
    std::shared_ptr<const SettingsConstraints> getSettingsConstraints() const;
>>>>>>> 880007787b... Introduce SettingsProoofile as a new access entity type.:dbms/src/Access/AccessRightsContext.h

    /// Checks if a specified access is granted, and throws an exception if not.
    /// Empty database means the current database.
    void checkAccess(const AccessFlags & access) const;
    void checkAccess(const AccessFlags & access, const std::string_view & database) const;
    void checkAccess(const AccessFlags & access, const std::string_view & database, const std::string_view & table) const;
    void checkAccess(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::string_view & column) const;
    void checkAccess(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::vector<std::string_view> & columns) const;
    void checkAccess(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const Strings & columns) const;
    void checkAccess(const AccessRightsElement & access) const;
    void checkAccess(const AccessRightsElements & access) const;

    /// Checks if a specified access is granted.
    bool isGranted(const AccessFlags & access) const;
    bool isGranted(const AccessFlags & access, const std::string_view & database) const;
    bool isGranted(const AccessFlags & access, const std::string_view & database, const std::string_view & table) const;
    bool isGranted(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::string_view & column) const;
    bool isGranted(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::vector<std::string_view> & columns) const;
    bool isGranted(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const Strings & columns) const;
    bool isGranted(const AccessRightsElement & access) const;
    bool isGranted(const AccessRightsElements & access) const;

    /// Checks if a specified access is granted, and logs a warning if not.
    bool isGranted(Poco::Logger * log_, const AccessFlags & access) const;
    bool isGranted(Poco::Logger * log_, const AccessFlags & access, const std::string_view & database) const;
    bool isGranted(Poco::Logger * log_, const AccessFlags & access, const std::string_view & database, const std::string_view & table) const;
    bool isGranted(Poco::Logger * log_, const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::string_view & column) const;
    bool isGranted(Poco::Logger * log_, const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::vector<std::string_view> & columns) const;
    bool isGranted(Poco::Logger * log_, const AccessFlags & access, const std::string_view & database, const std::string_view & table, const Strings & columns) const;
    bool isGranted(Poco::Logger * log_, const AccessRightsElement & access) const;
    bool isGranted(Poco::Logger * log_, const AccessRightsElements & access) const;

    /// Checks if a specified access is granted with grant option, and throws an exception if not.
    void checkGrantOption(const AccessFlags & access) const;
    void checkGrantOption(const AccessFlags & access, const std::string_view & database) const;
    void checkGrantOption(const AccessFlags & access, const std::string_view & database, const std::string_view & table) const;
    void checkGrantOption(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::string_view & column) const;
    void checkGrantOption(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const std::vector<std::string_view> & columns) const;
    void checkGrantOption(const AccessFlags & access, const std::string_view & database, const std::string_view & table, const Strings & columns) const;
    void checkGrantOption(const AccessRightsElement & access) const;
    void checkGrantOption(const AccessRightsElements & access) const;

    /// Checks if a specified role is granted with admin option, and throws an exception if not.
    void checkAdminOption(const UUID & role_id) const;

private:
    friend class AccessControlManager;
    ContextAccess(const AccessControlManager & manager_, const Params & params_); /// AccessRightsContext should be created by AccessRightsContextFactory.

    void setUser(const UserPtr & user_) const;
<<<<<<< HEAD:dbms/src/Access/ContextAccess.h
    void setRolesInfo(const std::shared_ptr<const EnabledRolesInfo> & roles_info_) const;
=======
    void setRolesInfo(const CurrentRolesInfoPtr & roles_info_) const;
    void setSettingsAndConstraints() const;
>>>>>>> 880007787b... Introduce SettingsProoofile as a new access entity type.:dbms/src/Access/AccessRightsContext.h

    template <int mode, bool grant_option, typename... Args>
    bool checkAccessImpl(Poco::Logger * log_, const AccessFlags & access, const Args &... args) const;

    template <int mode, bool grant_option>
    bool checkAccessImpl(Poco::Logger * log_, const AccessRightsElement & access) const;

    template <int mode, bool grant_option>
    bool checkAccessImpl(Poco::Logger * log_, const AccessRightsElements & access) const;

    boost::shared_ptr<const AccessRights> calculateResultAccess(bool grant_option) const;
    boost::shared_ptr<const AccessRights> calculateResultAccess(bool grant_option, UInt64 readonly_, bool allow_ddl_, bool allow_introspection_) const;

    const AccessControlManager * manager = nullptr;
    const Params params;
    mutable Poco::Logger * trace_log = nullptr;
    mutable UserPtr user;
    mutable String user_name;
    mutable ext::scope_guard subscription_for_user_change;
    mutable std::shared_ptr<const EnabledRoles> enabled_roles;
    mutable ext::scope_guard subscription_for_roles_changes;
    mutable std::shared_ptr<const EnabledRolesInfo> roles_info;
    mutable boost::atomic_shared_ptr<const boost::container::flat_set<UUID>> roles_with_admin_option;
    mutable boost::atomic_shared_ptr<const AccessRights> result_access_cache[7];
<<<<<<< HEAD:dbms/src/Access/ContextAccess.h
    mutable std::shared_ptr<const EnabledRowPolicies> enabled_row_policies;
    mutable std::shared_ptr<const EnabledQuota> enabled_quota;
=======
    mutable RowPolicyContextPtr row_policy_context;
    mutable QuotaContextPtr quota_context;

    mutable std::shared_ptr<const SettingsProfilesWatcher> settings_profiles_watcher;
    mutable ext::scope_guard subscription_for_settings_profiles_change;
    mutable ext::scope_guard subscription_for_default_profile_change;
    mutable std::shared_ptr<const Settings> default_settings;
    mutable std::shared_ptr<const SettingsConstraints> settings_constraints;

>>>>>>> 880007787b... Introduce SettingsProoofile as a new access entity type.:dbms/src/Access/AccessRightsContext.h
    mutable std::mutex mutex;
};

}
