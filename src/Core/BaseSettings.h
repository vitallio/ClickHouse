#pragma once

#include <Core/SettingsFields.h>
#include <Common/SettingsChanges.h>
#include <Common/FieldVisitors.h>
#include <ext/range.h>
#include <unordered_map>


namespace DB
{
class ReadBuffer;
class WriteBuffer;

enum class SettingsWriteFormat
{
    BINARY,             /// Part of the settings are serialized as strings, and other part as variants. This is the old behaviour.
    STRINGS_WITH_FLAGS, /// All settings are serialized as strings. Before each value the flag `is_important` is serialized.
    DEFAULT = STRINGS_WITH_FLAGS,
};


/** Template class to define collections of settings.
  * Example of usage:
  *
  * mysettings.h:
  * #define APPLY_FOR_MYSETTINGS(M) \
  *     M(UInt64, a, 100, "Description of a", 0) \
  *     M(Float, f, 3.11, "Description of f", IMPORTANT) // IMPORTANT - means the setting can't be ignored by older versions) \
  *     M(String, s, "default", "Description of s", 0)
  *
  * DECLARE_SETTINGS_TRAITS(MySettingsTraits, APPLY_FOR_MYSETTINGS)

  * struct MySettings : public BaseSettings<MySettingsTraits>
  * {
  * };
  *
  * mysettings.cpp:
  * IMPLEMENT_SETTINGS_TRAITS(MySettingsTraits, APPLY_FOR_MYSETTINGS)
  */
template <class Traits_>
class BaseSettings : public Traits_::Data
{
    using CustomSettingField = SettingFieldString;
    using CustomSettingMap = std::unordered_map<std::string_view, std::pair<std::shared_ptr<const String>, CustomSettingField>>;

public:
    using Traits = Traits_;

    void set(const std::string_view & name, const Field & value);
    Field get(const std::string_view & name) const;

    void setString(const std::string_view & name, const String & value);
    String getString(const std::string_view & name) const;

    const char * getTypeName(const std::string_view & name) const;

    bool canGet(const std::string_view & name) const;
    bool tryGet(const std::string_view & name, Field & value) const;
    bool tryGetString(const std::string_view & name, String & value) const;

    static bool canSet(const std::string_view & name);
    static bool canSet(const std::string_view & name, const Field & value);
    static bool canSetString(const std::string_view & name, const String & value);

    bool isChanged(const std::string_view & name) const;
    SettingsChanges changes() const;
    void applyChange(const SettingChange & change);
    void applyChanges(const SettingsChanges & changes);
    void applyChanges(const BaseSettings & changes);

    void resetToDefault();

    static const char * getDescription(const std::string_view & name);
    static Field castValue(const std::string_view & name, const Field & value);
    static String valueToString(const std::string_view & name, const Field & value);
    static Field stringToValue(const std::string_view & name, const String & value);

    // A debugging aid.
    std::string toString() const;

    void write(WriteBuffer & out, SettingsWriteFormat format = SettingsWriteFormat::DEFAULT) const;
    void read(ReadBuffer & in, SettingsWriteFormat format = SettingsWriteFormat::DEFAULT);

    class SettingFieldRef
    {
    public:
        SettingFieldRef(const typename Traits::Data & data_, const typename Traits::Accessor & accessor_, size_t index_) : data(&data_), accessor(&accessor_), index(index_) {}
        SettingFieldRef(const CustomSettingMap::mapped_type & custom_setting_);

        const String & getName() const;
        Field getValue() const;
        String getValueString() const;
        bool isValueChanged() const;
        const char * getTypeName() const;
        const char * getDescription() const;
        bool isCustom() const;

        bool operator==(const SettingFieldRef & other) const { return (getName() == other.getName()) && (getValue() == other.getValue()); }
        bool operator!=(const SettingFieldRef & other) const { return !(*this == other); }

    private:
        friend class BaseSettings;
        const typename Traits::Data * data = nullptr;
        const typename Traits::Accessor * accessor = nullptr;
        size_t index = 0;
        std::conditional_t<Traits::allow_custom_settings, const CustomSettingMap::mapped_type*, char> custom_setting = 0;
    };

    class Iterator
    {
    public:
        Iterator(const BaseSettings & settings_, const typename Traits::Accessor & accessor_, bool skip_changed_, bool skip_unchanged_);
        Iterator & operator++();
        Iterator operator++(int);
        SettingFieldRef operator *() const;

        bool operator ==(const Iterator & other) const;
        bool operator !=(const Iterator & other) const { return !(*this == other); }

    private:
        void doSkip();

        const BaseSettings * settings = nullptr;
        const typename Traits::Accessor * accessor = nullptr;
        size_t index = 0;
        std::conditional_t<Traits::allow_custom_settings, CustomSettingMap::const_iterator, char> custom_settings_iterator;
        bool skip_changed = false;
        bool skip_unchanged = false;
    };

    class Range
    {
    public:
        Range(const BaseSettings & settings_, bool including_changed_, bool including_unchanged_) : settings(settings_), accessor(Traits::Accessor::instance()), including_changed(including_changed_), including_unchanged(including_unchanged_) {}
        Iterator begin() const { return Iterator(settings, accessor, !including_changed, !including_unchanged); }
        Iterator end() const { return Iterator(settings, accessor, true, true); }

    private:
        const BaseSettings & settings;
        const typename Traits::Accessor & accessor;
        bool including_changed = true;
        bool including_unchanged = true;
    };

    Range all(bool including_changed = true, bool including_unchanged = true) const { return Range{*this, including_changed, including_unchanged}; }
    Range allChanged() const { return all(true, false); }
    Range allUnchanged() const { return all(false, true); }

    Iterator begin() const { return allChanged().begin(); }
    Iterator end() const { return allChanged().end(); }

private:
    SettingFieldString & getCustomSetting(const std::string_view & name);
    const SettingFieldString & getCustomSetting(const std::string_view & name) const;
    const SettingFieldString * tryGetCustomSetting(const std::string_view & name) const;

    std::conditional_t<Traits::allow_custom_settings, CustomSettingMap, char> custom_settings_map;
};

struct BaseSettingsHelpers
{
    static void throwSettingNotFound(const std::string_view & name);
    static void warningSettingNotFound(const std::string_view & name);

    static void writeString(const std::string_view & str, WriteBuffer & out);
    static String readString(ReadBuffer & in);

    enum Flags : UInt64
    {
        IMPORTANT = 0x01,
        CUSTOM = 0x02,
    };
    static void writeFlags(Flags flags, WriteBuffer & out);
    static Flags readFlags(ReadBuffer & in);
};

template <typename Traits_>
void BaseSettings<Traits_>::set(const std::string_view & name, const Field & value)
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        accessor.setValue(*this, index, value);
    else
        getCustomSetting(name) = value;
}

template <typename Traits_>
Field BaseSettings<Traits_>::get(const std::string_view & name) const
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        return accessor.getValue(*this, index);
    else
        return static_cast<Field>(getCustomSetting(name));
}

template <typename Traits_>
void BaseSettings<Traits_>::setString(const std::string_view & name, const String & value)
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        accessor.setValueString(*this, index, value);
    else
        getCustomSetting(name).parseFromString(value);
}

template <typename Traits_>
String BaseSettings<Traits_>::getString(const std::string_view & name) const
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        return accessor.getValueString(*this, index);
    else
        return getCustomSetting(name).toString();
}

template <typename Traits_>
const char * BaseSettings<Traits_>::getTypeName(const std::string_view & name) const
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        return accessor.getTypeName(index);
    else
        return "String";
}

template <typename Traits_>
bool BaseSettings<Traits_>::canGet(const std::string_view & name) const
{
    const auto & accessor = Traits::Accessor::instance();
    return (accessor.find(name) != static_cast<size_t>(-1)) || tryGetCustomSetting(name);
}

template <typename Traits_>
bool BaseSettings<Traits_>::tryGet(const std::string_view & name, Field & value) const
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
    {
        value = accessor.getValue(*this, index);
        return true;
    }
    if (const auto * custom_setting = tryGetCustomSetting(name))
    {
        value = static_cast<Field>(*custom_setting);
        return true;
    }
    return false;
}

template <typename Traits_>
bool BaseSettings<Traits_>::tryGetString(const std::string_view & name, String & value) const
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
    {
        value = accessor.getValueString(*this, index);
        return true;
    }
    if (const auto * custom_setting = tryGetCustomSetting(name))
    {
        value = custom_setting->toString();
        return true;
    }
    return false;
}

template <typename Traits_>
bool BaseSettings<Traits_>::canSet(const std::string_view & name)
{
    if constexpr (Traits::allow_custom_settings)
        return true;
    else
    {
        const auto & accessor = Traits::Accessor::instance();
        return accessor.find(name) != static_cast<size_t>(-1);
    }
}

template <typename Traits_>
bool BaseSettings<Traits_>::canSet(const std::string_view & name, const Field & value)
{
    const auto & accessor = Traits::Accessor::instance();
    size_t index = accessor.find(name);
    if (index == static_cast<size_t>(-1))
        return Traits::allow_custom_settings;
    try
    {
        accessor.castValue(index, value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

template <typename Traits_>
bool BaseSettings<Traits_>::canSetString(const std::string_view & name, const String & value)
{
    const auto & accessor = Traits::Accessor::instance();
    size_t index = accessor.find(name);
    if (index == static_cast<size_t>(-1))
        return Traits::allow_custom_settings;
    try
    {
        accessor.stringToValue(index, value);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

template <typename Traits_>
SettingFieldString & BaseSettings<Traits_>::getCustomSetting(const std::string_view & name)
{
    if constexpr (Traits::allow_custom_settings)
    {
        auto it = custom_settings_map.find(name);
        if (it == custom_settings_map.end())
        {
            auto new_name = std::make_shared<String>(name);
            it = custom_settings_map.emplace(*new_name, std::make_pair(new_name, CustomSettingField{})).first;
        }
        return it->second.second;
    }
    BaseSettingsHelpers::throwSettingNotFound(name);
}

template <typename Traits_>
const SettingFieldString & BaseSettings<Traits_>::getCustomSetting(const std::string_view & name) const
{
    if constexpr (Traits::allow_custom_settings)
    {
        auto it = custom_settings_map.find(name);
        if (it != custom_settings_map.end())
            return it->second.second;
    }
    BaseSettingsHelpers::throwSettingNotFound(name);
}

template <typename Traits_>
const SettingFieldString * BaseSettings<Traits_>::tryGetCustomSetting(const std::string_view & name) const
{
    if constexpr (Traits::allow_custom_settings)
    {
        auto it = custom_settings_map.find(name);
        if (it != custom_settings_map.end())
            return &it->second.second;
    }
    return nullptr;
}

template <typename Traits_>
BaseSettings<Traits_>::Iterator::Iterator(const BaseSettings & settings_, const typename Traits::Accessor & accessor_, bool skip_changed_, bool skip_unchanged_)
    : settings(&settings_), accessor(&accessor_), skip_changed(skip_changed_), skip_unchanged(skip_unchanged_)
{
    if constexpr (Traits::allow_custom_settings)
        custom_settings_iterator = settings->custom_settings_map.begin();
    doSkip();
}

template <typename Traits_>
typename BaseSettings<Traits_>::Iterator & BaseSettings<Traits_>::Iterator::operator++()
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (index == accessor->size())
            ++custom_settings_iterator;
        else
            ++index;
    }
    else
        ++index;
    doSkip();
    return *this;
}

template <typename Traits_>
typename BaseSettings<Traits_>::Iterator BaseSettings<Traits_>::Iterator::operator++(int)
{
    auto res = *this;
    ++*this;
    return res;
}

template <typename Traits_>
typename BaseSettings<Traits_>::SettingFieldRef BaseSettings<Traits_>::Iterator::operator*() const
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (index == accessor->size())
            return {custom_settings_iterator->second};
    }
    return {*settings, *accessor, index};
}

template <typename Traits_>
void BaseSettings<Traits_>::Iterator::doSkip()
{
    assert (index <= accessor->size());
    if (skip_unchanged)
    {
        if (skip_changed)
        {
            index = accessor->size();
            if constexpr (Traits::allow_custom_settings)
                custom_settings_iterator = settings->custom_settings_map.end();
            return;
        }

        while ((index != accessor->size()) && !accessor->isValueChanged(*settings, index))
            ++index;
        return;
    }

    if (skip_changed)
    {
        while ((index != accessor->size()) && accessor->isValueChanged(*settings, index))
            ++index;
        if constexpr (Traits::allow_custom_settings)
        {
            if (index == accessor->size())
                custom_settings_iterator = settings->custom_settings_map.end();
        }
    }
}

template <typename Traits_>
bool BaseSettings<Traits_>::Iterator::operator ==(const typename BaseSettings<Traits_>::Iterator & other) const
{
    if constexpr (Traits_::allow_custom_settings)
    {
        if (custom_settings_iterator != other.custom_settings_iterator)
            return false;
    }
    return ((index == other.index) && (settings == other.settings));
}

template <typename Traits_>
BaseSettings<Traits_>::SettingFieldRef::SettingFieldRef(const CustomSettingMap::mapped_type & custom_setting_)
{
    if constexpr (Traits_::allow_custom_settings)
        custom_setting = &custom_setting_;
    else
        assert(false);
}

template <typename Traits_>
const String & BaseSettings<Traits_>::SettingFieldRef::getName() const
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (custom_setting)
            return *custom_setting->first;
    }
    return accessor->getName(index);
}

template <typename Traits_>
Field BaseSettings<Traits_>::SettingFieldRef::getValue() const
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (custom_setting)
            return static_cast<Field>(custom_setting->second);
    }
    return accessor->getValue(*data, index);
}

template <typename Traits_>
String BaseSettings<Traits_>::SettingFieldRef::getValueString() const
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (custom_setting)
            return custom_setting->second.toString();
    }
    return accessor->getValueString(*data, index);
}

template <typename Traits_>
bool BaseSettings<Traits_>::SettingFieldRef::isValueChanged() const
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (custom_setting)
            return true;
    }
    return accessor->isValueChanged(*data, index);
}

template <typename Traits_>
const char * BaseSettings<Traits_>::SettingFieldRef::getTypeName() const
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (custom_setting)
            return "String";
    }
    return accessor->getTypeName(index);
}

template <typename Traits_>
const char * BaseSettings<Traits_>::SettingFieldRef::getDescription() const
{
    if constexpr (Traits::allow_custom_settings)
    {
        if (custom_setting)
            return "";
    }
    return accessor->getDescription(index);
}

template <typename Traits_>
bool BaseSettings<Traits_>::SettingFieldRef::isCustom() const
{
    if constexpr (Traits::allow_custom_settings)
        return custom_setting != nullptr;
    else
        return false;
}

template <typename Traits_>
bool BaseSettings<Traits_>::isChanged(const std::string_view & name) const
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        return accessor.isValueChanged(*this, index);
    return tryGetCustomSetting(name) != nullptr;
}

template <typename Traits_>
SettingsChanges BaseSettings<Traits_>::changes() const
{
    SettingsChanges res;
    for (const auto & field : *this)
        res.emplace_back(field.getName(), field.getValue());
    return res;
}

template <typename Traits_>
void BaseSettings<Traits_>::applyChange(const SettingChange & change)
{
    set(change.name, change.value);
}

template <typename Traits_>
void BaseSettings<Traits_>::applyChanges(const SettingsChanges & changes)
{
    for (const auto & change : changes)
        applyChange(change);
}

template <typename Traits_>
void BaseSettings<Traits_>::applyChanges(const BaseSettings & other_settings)
{
    for (const auto & field : other_settings)
        set(field.getName(), field.getValue());
}

template <typename Traits_>
void BaseSettings<Traits_>::resetToDefault()
{
    const auto & accessor = Traits::Accessor::instance();
    for (size_t i : ext::range(accessor.size()))
    {
        if (accessor.isValueChanged(*this, i))
            accessor.resetValueToDefault(*this, i);
    }

    if constexpr (Traits::allow_custom_settings)
        custom_settings_map.clear();
}


template <typename Traits_>
bool operator==(const BaseSettings<Traits_> & left, const BaseSettings<Traits_> & right)
{
    auto l = left.begin();
    for (const auto & r : right)
    {
        if ((l == left.end()) || (*l != r))
            return false;
        ++l;
    }
    return l == left.end();
}

template <typename Traits_>
bool operator!=(const BaseSettings<Traits_> & left, const BaseSettings<Traits_> & right)
{
    return !(left == right);
}


template <typename Traits_>
const char * BaseSettings<Traits_>::getDescription(const std::string_view & name)
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        return accessor.getDescription(index);
    else
        return "";
}

template <typename Traits_>
Field BaseSettings<Traits_>::castValue(const std::string_view & name, const Field & value)
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        return accessor.castValue(index, value);
    else
        return value;
}

template <typename Traits_>
String BaseSettings<Traits_>::valueToString(const std::string_view & name, const Field & value)
{
    const auto & accessor = Traits::Accessor::instance();
    if (size_t index = accessor.find(name); index != static_cast<size_t>(-1))
        return accessor.valueToString(index, value);
    else
        return applyVisitor(FieldVisitorToString(), value);
}

template <typename Traits_>
String BaseSettings<Traits_>::toString() const
{
    String res;
    for (const auto & field : *this)
    {
        if (!res.empty())
            res += ", ";
        res += field.getName() + " = " + applyVisitor(FieldVisitorDump{}, field.getValue());
    }
    return res;
}

template <typename Traits_>
void BaseSettings<Traits_>::write(WriteBuffer & out, SettingsWriteFormat format) const
{
    const auto & accessor = Traits::Accessor::instance();

    for (auto field : *this)
    {
        BaseSettingsHelpers::writeString(field.getName(), out);
        if (format >= SettingsWriteFormat::STRINGS_WITH_FLAGS)
        {
            using Flags = BaseSettingsHelpers::Flags;
            Flags flags{0};
            if (accessor.isImportant(field.index))
                flags = static_cast<Flags>(flags | Flags::IMPORTANT);
            if (field.isCustom())
                flags = static_cast<Flags>(flags | Flags::CUSTOM);
            BaseSettingsHelpers::writeFlags(flags, out);
            BaseSettingsHelpers::writeString(accessor.getValueString(*this, field.index), out);
        }
        else
            accessor.writeBinary(*this, field.index, out);
    }

    /// Empty string is a marker of the end of settings.
    BaseSettingsHelpers::writeString(std::string_view{}, out);
}

template <typename Traits_>
void BaseSettings<Traits_>::read(ReadBuffer & in, SettingsWriteFormat format)
{
    resetToDefault();
    const auto & accessor = Traits::Accessor::instance();
    while (true)
    {
        String name = BaseSettingsHelpers::readString(in);
        if (name.empty() /* empty string is a marker of the end of settings */)
            break;
        size_t index = accessor.find(name);

        using Flags = BaseSettingsHelpers::Flags;
        Flags flags{0};
        if (format >= SettingsWriteFormat::STRINGS_WITH_FLAGS)
            flags = BaseSettingsHelpers::readFlags(in);
        bool is_important = (flags & Flags::IMPORTANT);
        bool is_custom = (flags & Flags::CUSTOM);

        if (index != static_cast<size_t>(-1))
        {
            if (is_custom)
            {
                CustomSettingField temp;
                temp.parseFromString(BaseSettingsHelpers::readString(in));
                accessor.setValue(*this, index, static_cast<Field>(temp));
            }
            else if (format >= SettingsWriteFormat::STRINGS_WITH_FLAGS)
                accessor.setValueString(*this, index, BaseSettingsHelpers::readString(in));
            else
                accessor.readBinary(*this, index, in);
        }
        else if (is_custom && Traits::allow_custom_settings)
        {
            getCustomSetting(name).parseFromString(BaseSettingsHelpers::readString(in));
        }
        else if (is_important)
        {
            BaseSettingsHelpers::throwSettingNotFound(name);
        }
        else
        {
            BaseSettingsHelpers::warningSettingNotFound(name);
            BaseSettingsHelpers::readString(in);
        }
    }
}

#define DECLARE_SETTINGS_TRAITS(SETTINGS_TRAITS_NAME, LIST_OF_SETTINGS_MACRO) \
    DECLARE_SETTINGS_TRAITS_COMMON(SETTINGS_TRAITS_NAME, LIST_OF_SETTINGS_MACRO, 0)

#define DECLARE_SETTINGS_TRAITS_ALLOW_CUSTOM_SETTINGS(SETTINGS_TRAITS_NAME, LIST_OF_SETTINGS_MACRO) \
    DECLARE_SETTINGS_TRAITS_COMMON(SETTINGS_TRAITS_NAME, LIST_OF_SETTINGS_MACRO, 1)

#define DECLARE_SETTINGS_TRAITS_COMMON(SETTINGS_TRAITS_NAME, LIST_OF_SETTINGS_MACRO, ALLOW_CUSTOM_SETTINGS) \
    struct SETTINGS_TRAITS_NAME \
    { \
        struct Data \
        { \
            LIST_OF_SETTINGS_MACRO(DECLARE_SETTINGS_TRAITS_) \
        }; \
        \
        class Accessor \
        { \
        public: \
            static const Accessor & instance(); \
            size_t size() const { return field_infos.size(); } \
            size_t find(const std::string_view & name) const; \
            size_t getIndex(const std::string_view & name) const; \
            const String & getName(size_t index) const { return field_infos[index].name; } \
            const char * getTypeName(size_t index) const { return field_infos[index].type; } \
            const char * getDescription(size_t index) const { return field_infos[index].description; } \
            bool isImportant(size_t index) const { return field_infos[index].is_important; } \
            Field castValue(size_t index, const Field & value) const { return field_infos[index].cast_value_function(value); } \
            String valueToString(size_t index, const Field & value) const { return field_infos[index].value_to_string_function(value); } \
            Field stringToValue(size_t index, const String & str) const { return field_infos[index].string_to_value_function(str); } \
            void setValue(Data & data, size_t index, const Field & value) const { return field_infos[index].set_value_function(data, value); } \
            Field getValue(const Data & data, size_t index) const { return field_infos[index].get_value_function(data); } \
            void setValueString(Data & data, size_t index, const String & str) const { return field_infos[index].set_value_string_function(data, str); } \
            String getValueString(const Data & data, size_t index) const { return field_infos[index].get_value_string_function(data); } \
            bool isValueChanged(const Data & data, size_t index) const { return field_infos[index].is_value_changed_function(data); } \
            void resetValueToDefault(Data & data, size_t index) const { return field_infos[index].reset_value_to_default_function(data); } \
            void writeBinary(const Data & data, size_t index, WriteBuffer & out) const { return field_infos[index].write_binary_function(data, out); } \
            void readBinary(Data & data, size_t index, ReadBuffer & in) const { return field_infos[index].read_binary_function(data, in); } \
        \
        private: \
            Accessor(); \
            struct FieldInfo \
            { \
                String name; \
                const char * type; \
                const char * description; \
                bool is_important; \
                Field (*cast_value_function)(const Field &); \
                String (*value_to_string_function)(const Field &); \
                Field (*string_to_value_function)(const String &); \
                void (*set_value_function)(Data &, const Field &) ; \
                Field (*get_value_function)(const Data &) ; \
                void (*set_value_string_function)(Data &, const String &) ; \
                String (*get_value_string_function)(const Data &) ; \
                bool (*is_value_changed_function)(const Data &); \
                void (*reset_value_to_default_function)(Data &) ; \
                void (*write_binary_function)(const Data &, WriteBuffer &) ; \
                void (*read_binary_function)(Data &, ReadBuffer &) ; \
            }; \
            std::vector<FieldInfo> field_infos; \
            std::unordered_map<std::string_view, size_t> name_to_index_map; \
        }; \
        static constexpr bool allow_custom_settings = ALLOW_CUSTOM_SETTINGS; \
    };

#define DECLARE_SETTINGS_TRAITS_(TYPE, NAME, DEFAULT, DESCRIPTION, FLAGS) \
    SettingField##TYPE NAME {DEFAULT};

#define IMPLEMENT_SETTINGS_TRAITS(SETTINGS_TRAITS_NAME, LIST_OF_SETTINGS_MACRO) \
    const SETTINGS_TRAITS_NAME::Accessor & SETTINGS_TRAITS_NAME::Accessor::instance() \
    { \
        static const Accessor the_instance = [] \
        { \
            Accessor res; \
            constexpr int IMPORTANT = 1; \
            UNUSED(IMPORTANT); \
            LIST_OF_SETTINGS_MACRO(IMPLEMENT_SETTINGS_TRAITS_) \
            for (size_t i : ext::range(res.field_infos.size())) \
            { \
                const auto & info = res.field_infos[i]; \
                res.name_to_index_map.emplace(info.name, i); \
            } \
            return res; \
        }(); \
        return the_instance; \
    } \
    \
    SETTINGS_TRAITS_NAME::Accessor::Accessor() {} \
    \
    size_t SETTINGS_TRAITS_NAME::Accessor::find(const std::string_view & name) const \
    { \
        auto it = name_to_index_map.find(name); \
        if (it != name_to_index_map.end()) \
            return it->second; \
        return static_cast<size_t>(-1); \
    } \
    \
    size_t SETTINGS_TRAITS_NAME::Accessor::getIndex(const std::string_view & name) const \
    { \
        auto it = name_to_index_map.find(name); \
        if (it != name_to_index_map.end()) \
            return it->second; \
        BaseSettingsHelpers::throwSettingNotFound(name); \
    } \
    \
    template class BaseSettings<SETTINGS_TRAITS_NAME>;

//-V:IMPLEMENT_SETTINGS:501
#define IMPLEMENT_SETTINGS_TRAITS_(TYPE, NAME, DEFAULT, DESCRIPTION, FLAGS) \
    res.field_infos.emplace_back( \
        FieldInfo{#NAME, #TYPE, #DESCRIPTION, FLAGS & IMPORTANT, \
            [](const Field & value) -> Field { return static_cast<Field>(SettingField##TYPE{value}); }, \
            [](const Field & value) -> String { return SettingField##TYPE{value}.toString(); }, \
            [](const String & str) -> Field { SettingField##TYPE temp; temp.parseFromString(str); return static_cast<Field>(temp); }, \
            [](Data & data, const Field & value) { data.NAME = value; }, \
            [](const Data & data) -> Field { return static_cast<Field>(data.NAME); }, \
            [](Data & data, const String & str) { data.NAME.parseFromString(str); }, \
            [](const Data & data) -> String { return data.NAME.toString(); }, \
            [](const Data & data) -> bool { return data.NAME.changed; }, \
            [](Data & data) { data.NAME = SettingField##TYPE{DEFAULT}; }, \
            [](const Data & data, WriteBuffer & out) { data.NAME.writeBinary(out); }, \
            [](Data & data, ReadBuffer & in) { data.NAME.readBinary(in); } \
        });
}
