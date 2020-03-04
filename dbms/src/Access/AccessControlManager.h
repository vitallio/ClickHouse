#pragma once

#include <Access/MultipleAccessStorage.h>
#include <Poco/AutoPtr.h>
#include <memory>


namespace Poco
{
    namespace Net
    {
        class IPAddress;
    }
    namespace Util
    {
        class AbstractConfiguration;
    }
}

namespace DB
{
class ContextAccess;
struct User;
using UserPtr = std::shared_ptr<const User>;
class EnabledRoles;
class RoleCache;
class EnabledRowPolicies;
class RowPolicyCache;
class EnabledQuota;
class QuotaCache;
struct QuotaUsageInfo;
struct SettingsProfile;
using SettingsProfilePtr = std::shared_ptr<const SettingsProfile>;
class SettingsProfilesWatcher;
using SettingsProfilesWatcherPtr = std::shared_ptr<const SettingsProfilesWatcher>;
class SettingsProfilesCache;
class ClientInfo;
struct Settings;


/// Manages access control entities.
class AccessControlManager : public MultipleAccessStorage
{
public:
    AccessControlManager();
    ~AccessControlManager();

    void setLocalDirectory(const String & directory);
    void setUsersConfig(const Poco::Util::AbstractConfiguration & users_config);

    std::shared_ptr<const ContextAccess> getContextAccess(
        const UUID & user_id,
        const std::vector<UUID> & current_roles,
        bool use_default_roles,
        const Settings & settings,
        const String & current_database,
        const ClientInfo & client_info,
        const String & default_profile_name) const;

    std::shared_ptr<const EnabledRoles> getEnabledRoles(
        const std::vector<UUID> & current_roles,
        const std::vector<UUID> & current_roles_with_admin_option) const;

    std::shared_ptr<const EnabledRowPolicies> getEnabledRowPolicies(
        const UUID & user_id,
        const std::vector<UUID> & enabled_roles) const;

    std::shared_ptr<const EnabledQuota> getEnabledQuota(
        const String & user_name,
        const UUID & user_id,
        const std::vector<UUID> & enabled_roles,
        const Poco::Net::IPAddress & address,
        const String & custom_quota_key) const;

    std::vector<QuotaUsageInfo> getQuotaUsageInfo() const;

    SettingsProfilePtr getSettingsProfile(const String & name) const;
    SettingsProfilesWatcherPtr getSettingsProfilesWatcher(const UUID & user_id, const std::vector<UUID> & enabled_roles_) const;

private:
<<<<<<< HEAD
    class ContextAccessCache;
    std::unique_ptr<ContextAccessCache> context_access_cache;
    std::unique_ptr<RoleCache> role_cache;
    std::unique_ptr<RowPolicyCache> row_policy_cache;
    std::unique_ptr<QuotaCache> quota_cache;
=======
    std::unique_ptr<AccessRightsContextFactory> access_rights_context_factory;
    std::unique_ptr<RoleContextFactory> role_context_factory;
    std::unique_ptr<RowPolicyContextFactory> row_policy_context_factory;
    std::unique_ptr<QuotaContextFactory> quota_context_factory;
    std::unique_ptr<SettingsProfilesCache> settings_profiles_cache;
>>>>>>> 880007787b... Introduce SettingsProoofile as a new access entity type.
};

}
