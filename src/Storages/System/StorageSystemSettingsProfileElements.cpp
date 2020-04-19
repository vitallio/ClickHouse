#include <Storages/System/StorageSystemSettingsProfileElements.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnsNumber.h>
#include <Access/AccessControlManager.h>
#include <Access/Role.h>
#include <Access/User.h>
#include <Access/SettingsProfile.h>
#include <Interpreters/Context.h>
#include <boost/range/algorithm_ext/push_back.hpp>


namespace DB
{
namespace
{
    enum class OwnerType
    {
        USER,
        ROLE,
        SETTINGS_PROFILE,
    };

    DataTypeEnum8::Values getOwnerTypeEnumValues()
    {
        DataTypeEnum8::Values enum_values;
        enum_values.emplace_back("USER", static_cast<UInt8>(OwnerType::USER));
        enum_values.emplace_back("ROLE", static_cast<UInt8>(OwnerType::ROLE));
        enum_values.emplace_back("SETTINGS_PROFILE", static_cast<UInt8>(OwnerType::SETTINGS_PROFILE));
        return enum_values;
    }
}


NamesAndTypesList StorageSystemSettingsProfileElements::getNamesAndTypes()
{
    NamesAndTypesList names_and_types{
        {"owner_name", std::make_shared<DataTypeString>()},
        {"owner_type", std::make_shared<DataTypeEnum8>(getOwnerTypeEnumValues())},
        {"position", std::make_shared<DataTypeUInt64>()},
        {"setting_name", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeString>())},
        {"value", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeString>())},
        {"min", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeString>())},
        {"max", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeString>())},
        {"readonly", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt8>())},
        {"parent_profile", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeString>())},
    };
    return names_and_types;
}


void StorageSystemSettingsProfileElements::fillData(MutableColumns & res_columns, const Context & context, const SelectQueryInfo &) const
{
    context.checkAccess(AccessType::SHOW_USERS | AccessType::SHOW_ROLES | AccessType::SHOW_SETTINGS_PROFILES);
    const auto & access_control = context.getAccessControlManager();
    std::vector<UUID> ids = access_control.findAll<User>();
    boost::range::push_back(ids, access_control.findAll<Role>());
    boost::range::push_back(ids, access_control.findAll<SettingsProfile>());

    size_t column_index = 0;
    auto & column_owner_name = assert_cast<ColumnString &>(*res_columns[column_index++]);
    auto & column_owner_type = assert_cast<ColumnUInt8 &>(*res_columns[column_index++]).getData();
    auto & column_position = assert_cast<ColumnUInt64 &>(*res_columns[column_index++]).getData();
    auto & column_setting_name = assert_cast<ColumnString &>(assert_cast<ColumnNullable &>(*res_columns[column_index]).getNestedColumn());
    auto & column_setting_name_null_map = assert_cast<ColumnNullable &>(*res_columns[column_index++]).getNullMapData();
    auto & column_value = assert_cast<ColumnString &>(assert_cast<ColumnNullable &>(*res_columns[column_index]).getNestedColumn());
    auto & column_value_null_map = assert_cast<ColumnNullable &>(*res_columns[column_index++]).getNullMapData();
    auto & column_min = assert_cast<ColumnString &>(assert_cast<ColumnNullable &>(*res_columns[column_index]).getNestedColumn());
    auto & column_min_null_map = assert_cast<ColumnNullable &>(*res_columns[column_index++]).getNullMapData();
    auto & column_max = assert_cast<ColumnString &>(assert_cast<ColumnNullable &>(*res_columns[column_index]).getNestedColumn());
    auto & column_max_null_map = assert_cast<ColumnNullable &>(*res_columns[column_index++]).getNullMapData();
    auto & column_readonly = assert_cast<ColumnUInt8 &>(assert_cast<ColumnNullable &>(*res_columns[column_index]).getNestedColumn()).getData();
    auto & column_readonly_null_map = assert_cast<ColumnNullable &>(*res_columns[column_index++]).getNullMapData();
    auto & column_parent_profile = assert_cast<ColumnString &>(assert_cast<ColumnNullable &>(*res_columns[column_index]).getNestedColumn());
    auto & column_parent_profile_null_map = assert_cast<ColumnNullable &>(*res_columns[column_index++]).getNullMapData();

    auto add_rows_for_single_element = [&](const String & owner_name, OwnerType owner_type, const SettingsProfileElement & element, size_t & position)
    {
        if (element.parent_profile)
        {
            auto parent_profile = access_control.tryReadName(*element.parent_profile);
            if (parent_profile)
            {
                column_owner_name.insertData(owner_name.data(), owner_name.length());
                column_owner_type.push_back(owner_type);
                column_position.push_back(position++);
                column_setting_name.insertDefault();
                column_setting_name_null_map.push_back(true);
                column_value.insertDefault();
                column_value_null_map.push_back(true);
                column_min.insertDefault();
                column_min_null_map.push_back(true);
                column_max.insertDefault();
                column_max_null_map.push_back(true);
                column_readonly.push_back(0);
                column_readonly_null_map.push_back(true);
                column_parent_profile.insertData(parent_profile->data(), parent_profile->length());
                column_parent_profile_null_map.push_back(false);
            }
        }

        if ((element.setting_index != static_cast<size_t>(-1))
            && (!element.value.isNull() || !element.min_value.isNull() || !element.max_value.isNull() || element.readonly))
        {
            auto setting_name = Settings::getName(element.setting_index);
            column_owner_name.insertData(owner_name.data(), owner_name.length());
            column_owner_type.push_back(owner_type);
            column_position.push_back(position++);
            column_setting_name.insertData(setting_name.data, setting_name.size);
            column_setting_name_null_map.push_back(false);

            if (element.value.isNull())
            {
                column_value.insertDefault();
                column_value_null_map.push_back(true);
            }
            else
            {
                String str = Settings::valueToString(element.setting_index, element.value);
                column_value.insertData(str.data(), str.length());
                column_value_null_map.push_back(false);
            }

            if (element.min_value.isNull())
            {
                column_min.insertDefault();
                column_min_null_map.push_back(true);
            }
            else
            {
                String str = Settings::valueToString(element.setting_index, element.min_value);
                column_min.insertData(str.data(), str.length());
                column_min_null_map.push_back(false);
            }

            if (element.max_value.isNull())
            {
                column_max.insertDefault();
                column_max_null_map.push_back(true);
            }
            else
            {
                String str = Settings::valueToString(element.setting_index, element.max_value);
                column_max.insertData(str.data(), str.length());
                column_max_null_map.push_back(false);
            }

            if (element.readonly)
            {
                column_readonly.push_back(*element.readonly);
                column_readonly_null_map.push_back(false);
            }
            else
            {
                column_readonly.push_back(0);
                column_readonly_null_map.push_back(true);
            }

            column_parent_profile.insertDefault();
            column_parent_profile_null_map.push_back(true);
        }
    };

    auto add_rows = [&](const String & owner_name, OwnerType owner_type, const SettingsProfileElements & elements)
    {
        size_t position = 0;
        for (const auto & element : elements)
            add_rows_for_single_element(owner_name, owner_type, element, position);
    };

    for (const auto & id : ids)
    {
        auto entity = access_control.tryRead(id);
        if (!entity)
            continue;

        const String & owner = entity->getFullName();

        if (auto role = typeid_cast<RolePtr>(entity))
            add_rows(owner, OwnerType::ROLE, role->settings);
        else if (auto user = typeid_cast<UserPtr>(entity))
            add_rows(owner, OwnerType::USER, user->settings);
        else if (auto profile = typeid_cast<SettingsProfilePtr>(entity))
            add_rows(owner, OwnerType::SETTINGS_PROFILE, profile->elements);
        else
            continue;
    }
}

}
