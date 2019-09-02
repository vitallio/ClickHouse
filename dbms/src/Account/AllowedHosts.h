#pragma once

#include <Core/Types.h>
#include <Poco/Net/IPAddress.h>
#include <vector>
#include <memory>


namespace Poco
{
class RegularExpression;
}


namespace DB
{
/// Represents lists of hosts an user is allowed to connect to the server from.
class AllowedHosts
{
public:
    using IPAddress = Poco::Net::IPAddress;

    struct IPSubnet
    {
        IPAddress prefix;
        IPAddress mask;

        friend bool operator ==(const IPSubnet & lhs, const IPSubnet & rhs);
        friend bool operator !=(const IPSubnet & lhs, const IPSubnet & rhs) { return !(lhs == rhs); }
    };

    AllowedHosts();
    AllowedHosts(const AllowedHosts & src);
    AllowedHosts & operator =(const AllowedHosts & src);
    ~AllowedHosts();

    /// Removes contained hosts.
    void clear();

    /// Adds a host.
    void addIPAddress(const IPAddress & address);
    void addIPSubnet(const IPSubnet & subnet);
    void addIPSubnet(const IPAddress & prefix, const IPAddress & mask);
    void addIPSubnet(const IPAddress & prefix, size_t num_prefix_bits);
    void addHost(const String & host);
    void addHostRegexp(const String & host_regexp);

    const std::vector<IPAddress> & getIPAddresses() const { return ip_addresses; }
    const std::vector<IPSubnet> & getIPSubnets() const { return ip_subnets; }
    const std::vector<String> & getHosts() const { return hosts; }
    const std::vector<String> & getHostRegexps() const { return host_regexps; }

    /// Checks if the provided address is in the list. Returns false if not.
    bool contains(const IPAddress & address) const;

    /// Checks if the provided address is in the list. Throws an exception if not.
    void checkContains(const IPAddress & address) const;

    friend bool operator ==(const AllowedHosts & lhs, const AllowedHosts & rhs);
    friend bool operator !=(const AllowedHosts & lhs, const AllowedHosts & rhs) { return !(lhs == rhs); }

private:
    bool containsImpl(const IPAddress & address, std::exception_ptr & error) const;

    std::vector<IPAddress> ip_addresses;
    std::vector<IPSubnet> ip_subnets;
    std::vector<String> hosts;
    std::vector<String> host_regexps;
    mutable std::vector<std::unique_ptr<Poco::RegularExpression>> host_regexps_compiled;
};
}
