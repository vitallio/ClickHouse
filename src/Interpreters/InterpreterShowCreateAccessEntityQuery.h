#pragma once

#include <Interpreters/IInterpreter.h>
#include <Parsers/IAST_fwd.h>
#include <Core/UUID.h>


namespace DB
{
class AccessControlManager;
class Context;
class AccessRightsElements;
struct IAccessEntity;
using AccessEntityPtr = std::shared_ptr<const IAccessEntity>;


/** Returns a single item containing a statement which could be used to create a specified role.
  */
class InterpreterShowCreateAccessEntityQuery : public IInterpreter
{
public:
    InterpreterShowCreateAccessEntityQuery(const ASTPtr & query_ptr_, const Context & context_);

    BlockIO execute() override;

    bool ignoreQuota() const override { return true; }
    bool ignoreLimits() const override { return true; }

    static ASTPtr getCreateQuery(const IAccessEntity & entity, const Context & context);
    static ASTPtr getAttachQuery(const IAccessEntity & entity);

private:
    BlockInputStreamPtr executeImpl();
    std::vector<AccessEntityPtr> getEntities() const;
    ASTs getCreateQueries() const;

    ASTPtr query_ptr;
    const Context & context;
};


}
