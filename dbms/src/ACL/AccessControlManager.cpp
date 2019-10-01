#include <ACL/AccessControlManager.h>
#include <ACL/MultipleAttributesStorage.h>
#include <ACL/MemoryAttributesStorage.h>


namespace DB
{
namespace AccessControlNames
{
    extern const size_t ROLE_NAMESPACE_IDX = 0;
}


namespace
{
    std::vector<std::unique_ptr<IAttributesStorage>> createStorages()
    {
        std::vector<std::unique_ptr<IAttributesStorage>> list;
        list.emplace_back(std::make_unique<MemoryAttributesStorage>());
        return list;
    }
}


AccessControlManager::AccessControlManager()
    : MultipleAttributesStorage(createStorages())
{
}


AccessControlManager::~AccessControlManager()
{
}

}
