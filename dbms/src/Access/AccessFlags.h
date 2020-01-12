#pragma once

#include <Access/AccessType.h>
#include <Core/Types.h>
#include <Common/Exception.h>
#include <ext/range.h>
#include <bitset>
#include <optional>
#include <unordered_map>


namespace DB
{
/// Represents a combination of access types which can be granted on databases, tables, columns, etc.
/// For example "SELECT, CREATE USER" is an access type.
class AccessFlags
{
public:
    AccessFlags(AccessType type);

    /// The same as AccessFlags(AccessType::NONE).
    AccessFlags() = default;

    /// Constructs from a string like "SELECT".
    AccessFlags(const std::string_view & keyword);

    /// Constructs from a list of strings like "SELECT, UPDATE, INSERT".
    AccessFlags(const std::vector<std::string_view> & keywords);
    AccessFlags(const Strings & keywords);

    AccessFlags(const AccessFlags & src) = default;
    AccessFlags(AccessFlags && src) = default;
    AccessFlags & operator =(const AccessFlags & src) = default;
    AccessFlags & operator =(AccessFlags && src) = default;

    /// Returns the access type which contains two specified access types.
    AccessFlags & operator |=(const AccessFlags & other) { flags |= other.flags; return *this; }
    friend AccessFlags operator |(const AccessFlags & left, const AccessFlags & right) { return AccessFlags(left) |= right; }

    /// Returns the access type which contains the common part of two access types.
    AccessFlags & operator &=(const AccessFlags & other) { flags &= other.flags; return *this; }
    friend AccessFlags operator &(const AccessFlags & left, const AccessFlags & right) { return AccessFlags(left) &= right; }

    /// Returns the access type which contains only the part of the first access type which is not the part of the second access type.
    /// (lhs - rhs) is the same as (lhs & ~rhs).
    AccessFlags & operator -=(const AccessFlags & other) { flags &= ~other.flags; return *this; }
    friend AccessFlags operator -(const AccessFlags & left, const AccessFlags & right) { return AccessFlags(left) -= right; }

    AccessFlags operator ~() const { AccessFlags res; res.flags = ~flags; return res; }

    bool isEmpty() const { return flags.none(); }
    explicit operator bool() const { return isEmpty(); }
    bool contains(const AccessFlags & other) const { return (flags & other.flags) == other.flags; }

    friend bool operator ==(const AccessFlags & left, const AccessFlags & right) { return left.flags == right.flags; }
    friend bool operator !=(const AccessFlags & left, const AccessFlags & right) { return !(left == right); }

    void clear() { flags.reset(); }

    /// Returns a comma-separated list of keywords, like "SELECT, CREATE USER, UPDATE".
    String toString() const;

    /// Returns a list of keywords.
    std::vector<std::string_view> toKeywords() const;

    /// Returns the access types which could be granted on the database level.
    /// For example, SELECT can be granted on the database level, but CREATE_USER cannot.
    static AccessFlags allGrantableOnDatabaseLevel();

    /// Returns the access types which could be granted on the table/dictionary level.
    static AccessFlags allGrantableOnTableLevel();

    /// Returns the access types which could be granted on the column/attribute level.
    static AccessFlags allGrantableOnColumnLevel();

private:
    static constexpr size_t NUM_FLAGS = 64;
    using Flags = std::bitset<NUM_FLAGS>;
    Flags flags;

    AccessFlags(const Flags & flags_) : flags(flags_) {}

    template <typename = void>
    class Impl;
};


namespace ErrorCodes
{
    extern const int UNKNOWN_ACCESS_TYPE;
}

template <typename>
class AccessFlags::Impl
{
public:
    static const Impl & instance()
    {
        static const Impl res;
        return res;
    }

    Flags accessTypeToFlags(AccessType type) const
    {
        return access_type_to_flags_mapping[static_cast<size_t>(type)];
    }

    Flags keywordToFlags(const std::string_view & keyword) const
    {
        auto it = keyword_to_flags_map.find(keyword);
        if (it == keyword_to_flags_map.end())
        {
            String uppercased_keyword{keyword};
            boost::to_upper(uppercased_keyword);
            it = keyword_to_flags_map.find(uppercased_keyword);
            if (it == keyword_to_flags_map.end())
                throw Exception("Unknown access type: " + String(keyword), ErrorCodes::UNKNOWN_ACCESS_TYPE);
        }
        return it->second;
    }

    Flags keywordsToFlags(const std::vector<std::string_view> & keywords) const
    {
        Flags flags;
        for (const auto & keyword : keywords)
            flags |= keywordToFlags(keyword);
        return flags;
    }

    Flags keywordsToFlags(const Strings & keywords) const
    {
        Flags flags;
        for (const auto & keyword : keywords)
            flags |= keywordToFlags(keyword);
        return flags;
    }

    std::vector<std::string_view> flagsToKeywords(const Flags & flags) const
    {
        std::vector<std::string_view> keywords;
        flagsToKeywordsRec(flags, keywords, *flags_to_keyword_tree);

        if (keywords.empty())
            keywords.push_back("USAGE");

        return keywords;
    }

    String flagsToString(const Flags & flags) const
    {
        String str;
        for (const auto & keyword : flagsToKeywords(flags))
        {
            if (!str.empty())
                str += ", ";
            str += keyword;
        }
        return str;
    }

    const Flags & allGrantableOnDatabaseLevel() const { return all_grantable_on_level[DATABASE_LEVEL]; }
    const Flags & allGrantableOnTableLevel() const { return all_grantable_on_level[TABLE_LEVEL]; }
    const Flags & allGrantableOnColumnLevel() const { return all_grantable_on_level[COLUMN_LEVEL]; }

private:
    enum Level
    {
        GLOBAL_LEVEL = 0,
        DATABASE_LEVEL = 1,
        TABLE_LEVEL = 2,
        VIEW_LEVEL = 2,
        DICTIONARY_LEVEL = 2,
        COLUMN_LEVEL = 3,
    };

    struct Node;
    using NodePtr = std::unique_ptr<Node>;
    using Nodes = std::vector<NodePtr>;

    template <typename T>
    static void push_back(std::vector<T> &)
    {
    }

    template <typename T, typename... Args>
    static void push_back(std::vector<T> & vec, T first, Args&& ... other)
    {
        vec.push_back(std::move(first));
        push_back(vec, std::move(other)...);
    }

    template <typename... Args>
    static Nodes nodes(Args&& ... args)
    {
        Nodes res;
        push_back(res, std::move(args)...);
        return res;
    }

    struct Node
    {
        std::string_view keyword;
        std::vector<String> aliases;
        Flags flags;
        Level level = GLOBAL_LEVEL;
        Nodes children;

        Node(std::string_view keyword_, size_t flag_, Level level_)
            : keyword(keyword_), level(level_)
        {
            flags.set(flag_);
        }

        Node(std::string_view keyword_, Nodes children_)
            : keyword(keyword_), children(std::move(children_))
        {
            for (const auto & child : children)
            {
                flags |= child->flags;
                level = std::max(level, child->level);
            }
        }

        template <typename... Args>
        Node(std::string_view keyword_, NodePtr first_child, Args &&... other_children)
            : Node(keyword_, nodes(std::move(first_child), std::move(other_children)...)) {}
    };

    static void flagsToKeywordsRec(const Flags & flags, std::vector<std::string_view> & keywords, const Node & start_node)
    {
        Flags matching_flags = (flags & start_node.flags);
        if (matching_flags.any())
        {
            if (matching_flags == start_node.flags)
            {
                keywords.push_back(start_node.keyword);
            }
            else
            {
                for (const auto & child : start_node.children)
                   flagsToKeywordsRec(flags, keywords, *child);
            }
        }
    }

    static void makeFlagsToKeywordTree(NodePtr & flags_to_keyword_tree_)
    {
        size_t next_flag = 0;
        Nodes all;

        auto show = std::make_unique<Node>("SHOW", next_flag++, COLUMN_LEVEL);
        push_back(all, std::move(show));

        auto select = std::make_unique<Node>("SELECT", next_flag++, COLUMN_LEVEL);
        auto insert = std::make_unique<Node>("INSERT", next_flag++, COLUMN_LEVEL);
        auto update = std::make_unique<Node>("UPDATE", next_flag++, COLUMN_LEVEL);
        auto delet = std::make_unique<Node>("DELETE", next_flag++, COLUMN_LEVEL);
        push_back(all, std::move(select), std::move(insert), std::move(update), std::move(delet));

        flags_to_keyword_tree_ = std::make_unique<Node>("ALL", std::move(all));
        flags_to_keyword_tree_->aliases.push_back("ALL PRIVILEGES");
    }

    void makeKeywordToFlagsMap(std::unordered_map<std::string_view, Flags> & keyword_to_flags_map_, Node * start_node = nullptr)
    {
        if (!start_node)
        {
            start_node = flags_to_keyword_tree.get();
            keyword_to_flags_map_["USAGE"] = {};
            keyword_to_flags_map_["NONE"] = {};
            keyword_to_flags_map_["NO PRIVILEGES"] = {};
        }
        String uppercased(start_node->keyword);
        boost::to_upper(uppercased);
        if (uppercased == start_node->keyword)
            keyword_to_flags_map_[start_node->keyword] = start_node->flags;
        else
            start_node->aliases.push_back(uppercased);
        for (auto & alias : start_node->aliases)
        {
            boost::to_upper(alias);
            keyword_to_flags_map_[alias] = start_node->flags;
        }
        for (auto & child : start_node->children)
            makeKeywordToFlagsMap(keyword_to_flags_map_, child.get());
    }

    void makeAccessTypeToFlagsMapping(std::vector<Flags> & access_type_to_flags_mapping_)
    {
        access_type_to_flags_mapping_.resize(MAX_ACCESS_TYPE);
        for (auto access_type : ext::range_with_static_cast<AccessType>(0, MAX_ACCESS_TYPE))
            access_type_to_flags_mapping_[static_cast<size_t>(access_type)] = keyword_to_flags_map.at(::DB::toString(access_type));
    }

    void collectAllGrantableOnLevel(std::vector<Flags> & all_grantable_on_level_, const Node * start_node = nullptr)
    {
        if (!start_node)
        {
            start_node = flags_to_keyword_tree.get();
            all_grantable_on_level.resize(COLUMN_LEVEL + 1);
        }
        for (size_t i = 0; i <= start_node->level; ++i)
            all_grantable_on_level_[i] |= start_node->flags;
        for (const auto & child : start_node->children)
            collectAllGrantableOnLevel(all_grantable_on_level_, child.get());
    }

    Impl()
    {
        makeFlagsToKeywordTree(flags_to_keyword_tree);
        makeKeywordToFlagsMap(keyword_to_flags_map);
        makeAccessTypeToFlagsMapping(access_type_to_flags_mapping);
        collectAllGrantableOnLevel(all_grantable_on_level);
    }

    std::unique_ptr<Node> flags_to_keyword_tree;
    std::unordered_map<std::string_view, Flags> keyword_to_flags_map;
    std::vector<Flags> access_type_to_flags_mapping;
    std::vector<Flags> all_grantable_on_level;
};


inline AccessFlags::AccessFlags(AccessType type) : flags(Impl<>::instance().accessTypeToFlags(type)) {}
inline AccessFlags::AccessFlags(const std::string_view & keyword) : flags(Impl<>::instance().keywordToFlags(keyword)) {}
inline AccessFlags::AccessFlags(const std::vector<std::string_view> & keywords) : flags(Impl<>::instance().keywordsToFlags(keywords)) {}
inline AccessFlags::AccessFlags(const Strings & keywords) : flags(Impl<>::instance().keywordsToFlags(keywords)) {}
inline String AccessFlags::toString() const { return Impl<>::instance().flagsToString(flags); }
inline std::vector<std::string_view> AccessFlags::toKeywords() const { return Impl<>::instance().flagsToKeywords(flags); }
inline AccessFlags AccessFlags::allGrantableOnDatabaseLevel() { return Impl<>::instance().allGrantableOnDatabaseLevel(); }
inline AccessFlags AccessFlags::allGrantableOnTableLevel() { return Impl<>::instance().allGrantableOnTableLevel(); }
inline AccessFlags AccessFlags::allGrantableOnColumnLevel() { return Impl<>::instance().allGrantableOnColumnLevel(); }

inline AccessFlags operator |(AccessType left, AccessType right) { return AccessFlags(left) | right; }
inline AccessFlags operator &(AccessType left, AccessType right) { return AccessFlags(left) & right; }
inline AccessFlags operator -(AccessType left, AccessType right) { return AccessFlags(left) - right; }
inline AccessFlags operator ~(AccessType x) { return ~AccessFlags(x); }

}
