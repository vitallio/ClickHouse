#include <Account/AllowedHosts.h>
#include <Common/Exception.h>
#include <common/SimpleCache.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/RegularExpression.h>
#include <ext/scope_guard.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int DNS_ERROR;
    extern const int IP_ADDRESS_NOT_ALLOWED;
}

namespace
{
    using IPAddress = Poco::Net::IPAddress;

    IPAddress toIPv6(const IPAddress & addr)
    {
        if (addr.family() == IPAddress::IPv6)
            return addr;

        return IPAddress("::FFFF:" + addr.toString());
    }


    IPAddress maskToIPv6(const IPAddress & mask)
    {
        if (mask.family() == IPAddress::IPv6)
            return mask;

        return IPAddress(96, IPAddress::IPv6) | toIPv6(mask);
    }


    bool isAddressOfHostImpl(const IPAddress & address, const String & host)
    {
        IPAddress addr_v6 = toIPv6(address);

        /// Resolve by hand, because Poco don't use AI_ALL flag but we need it.
        addrinfo * ai = nullptr;
        SCOPE_EXIT(
        {
            if (ai)
                freeaddrinfo(ai);
        });

        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags |= AI_V4MAPPED | AI_ALL;

        int ret = getaddrinfo(host.c_str(), nullptr, &hints, &ai);
        if (0 != ret)
            throw Exception("Cannot getaddrinfo: " + std::string(gai_strerror(ret)), ErrorCodes::DNS_ERROR);

        for (; ai != nullptr; ai = ai->ai_next)
        {
            if (ai->ai_addrlen && ai->ai_addr)
            {
                if (ai->ai_family == AF_INET6)
                {
                    if (addr_v6 == IPAddress(
                        &reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr, sizeof(in6_addr),
                        reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_scope_id))
                    {
                        return true;
                    }
                }
                else if (ai->ai_family == AF_INET)
                {
                    if (addr_v6 == toIPv6(Poco::Net::IPAddress(
                        &reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr, sizeof(in_addr))))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }


    bool isAddressOfHost(const IPAddress & address, const String & host)
    {
        static SimpleCache<decltype(isAddressOfHostImpl), isAddressOfHostImpl> cache;
        return cache(address, host);
    }


    String getHostByAddressImpl(const IPAddress & address)
    {
        Poco::Net::SocketAddress sock_addr(address, 0);

        /// Resolve by hand, because Poco library doesn't have such functionality.
        char host[1024];
        int gai_errno = getnameinfo(sock_addr.addr(), sock_addr.length(), host, sizeof(host), nullptr, 0, NI_NAMEREQD);
        if (0 != gai_errno)
            throw Exception("Cannot getnameinfo: " + std::string(gai_strerror(gai_errno)), ErrorCodes::DNS_ERROR);

        if (!isAddressOfHost(address, host))
            throw Exception("Host " + String(host) + " isn't resolved back to " + address.toString(), ErrorCodes::DNS_ERROR);
        return host;
    }


    String getHostByAddress(const IPAddress & address)
    {
        static SimpleCache<decltype(getHostByAddressImpl), &getHostByAddressImpl> cache;
        return cache(address);
    }
}


bool operator==(const AllowedHosts::IPSubnet & lhs, const AllowedHosts::IPSubnet & rhs)
{
    return (lhs.prefix == rhs.prefix) && (lhs.mask == rhs.mask);
}


bool operator<(const AllowedHosts::IPSubnet & lhs, const AllowedHosts::IPSubnet & rhs)
{
    return (lhs.prefix < rhs.prefix) || ((lhs.prefix == rhs.prefix) && (lhs.mask < rhs.mask));
}


AllowedHosts::AllowedHosts() = default;
AllowedHosts::~AllowedHosts() = default;


AllowedHosts::AllowedHosts(const AllowedHosts & src)
{
    *this = src;
}


AllowedHosts & AllowedHosts::operator =(const AllowedHosts & src)
{
    ip_addresses = src.ip_addresses;
    ip_subnets = src.ip_subnets;
    hosts = src.hosts;
    host_regexps = src.host_regexps;
    host_regexps_compiled.clear();
    return *this;
}


void AllowedHosts::clear()
{
    ip_addresses.clear();
    ip_subnets.clear();
    hosts.clear();
    host_regexps.clear();
    host_regexps_compiled.clear();
}


void AllowedHosts::addIPAddress(const IPAddress & address)
{
    IPAddress addr_v6 = toIPv6(address);

    /// Keep the vector `ip_addresses` sorted to simplify the comparison.
    ip_addresses.insert(std::upper_bound(ip_addresses.begin(), ip_addresses.end(), addr_v6), addr_v6);
}


void AllowedHosts::addIPSubnet(const IPSubnet & subnet)
{
    IPSubnet subnet_v6;
    subnet_v6.mask = maskToIPv6(subnet.mask);

    if (subnet_v6.mask == Poco::Net::IPAddress(128, Poco::Net::IPAddress::IPv6))
    {
        addIPAddress(subnet.prefix);
        return;
    }

    subnet_v6.prefix = toIPv6(subnet.prefix) & subnet_v6.mask;

    /// Keep the vector `ip_subnets` sorted to simplify the comparison.
    ip_subnets.insert(std::upper_bound(ip_subnets.begin(), ip_subnets.end(), subnet_v6), subnet_v6);
}


void AllowedHosts::addIPSubnet(const IPAddress & prefix, const IPAddress & mask)
{
    addIPSubnet(IPSubnet{prefix, mask});
}


void AllowedHosts::addIPSubnet(const IPAddress & prefix, size_t num_prefix_bits)
{
    addIPSubnet(prefix, Poco::Net::IPAddress(num_prefix_bits, prefix.family()));
}


void AllowedHosts::addHost(const String & host)
{
    /// Keep the vector `hosts` sorted to simplify the comparison.
    hosts.insert(std::upper_bound(hosts.begin(), hosts.end(), host), host);
}


void AllowedHosts::addHostRegexp(const String & host_regexp)
{
    auto compiled_regexp = std::make_unique<Poco::RegularExpression>(host_regexp);
    auto new_pos_it = std::upper_bound(host_regexps.begin(), host_regexps.end(), host_regexp);
    size_t new_pos = new_pos_it - host_regexps.begin();
    host_regexps.insert(new_pos_it, host_regexp);
    host_regexps_compiled.insert(host_regexps_compiled.begin() + new_pos, std::move(compiled_regexp));
}


bool AllowedHosts::contains(const IPAddress & address) const
{
    std::exception_ptr error;
    return containsImpl(address, error);
}


void AllowedHosts::checkContains(const IPAddress & address) const
{
    std::exception_ptr error;
    if (containsImpl(address, error))
        return;

    if (error)
        std::rethrow_exception(error);
    throw Exception("You are not allowed to connect from address " + address.toString(), ErrorCodes::IP_ADDRESS_NOT_ALLOWED);
}


bool AllowedHosts::containsImpl(const IPAddress & address, std::exception_ptr & error) const
{
    IPAddress addr_v6 = toIPv6(address);

    /// Check `ip_addresses`.
    if (std::binary_search(ip_addresses.begin(), ip_addresses.end(), addr_v6))
        return true;

    /// Check `ip_subnets`.
    for (const auto & subnet : ip_subnets)
        if ((addr_v6 & subnet.mask) == subnet.prefix)
            return true;

    /// Check `hosts`.
    for (const String & host : hosts)
    {
        try
        {
            if (isAddressOfHost(address, host))
                return true;
        }
        catch (...)
        {
            if (!error)
                error = std::current_exception();
        }
    }

    /// Check `host_regexps`.
    if (!host_regexps.empty())
    {
        for (size_t i = 0; i != host_regexps.size(); ++i)
        {
            if (!host_regexps_compiled[i])
            {
                try
                {
                    host_regexps_compiled[i] = std::make_unique<Poco::RegularExpression>(host_regexps[i]);
                }
                catch (...)
                {
                    if (!error)
                        error = std::current_exception();
                }
            }
        }

        try
        {
            String resolved_host = getHostByAddress(address);
            for (const auto & compiled_regexp : host_regexps_compiled)
            {
                if (compiled_regexp && compiled_regexp->match(resolved_host))
                    return true;
            }
        }
        catch (...)
        {
            if (!error)
                error = std::current_exception();
        }
    }

    return false;
}


bool operator ==(const AllowedHosts & lhs, const AllowedHosts & rhs)
{
    return (lhs.ip_addresses == rhs.ip_addresses) && (lhs.ip_subnets == rhs.ip_subnets) && (lhs.hosts == rhs.hosts)
        && (lhs.host_regexps == rhs.host_regexps);
}
}
