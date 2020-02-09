#pragma once

#include <Parsers/IParserBase.h>


namespace DB
{
/** Parses a string like this:
  * {role|CURRENT_USER} [,...] | NONE | ALL | ALL EXCEPT {role|CURRENT_USER} [,...]
  */
class ParserRoleList : public IParserBase
{
public:
    ParserRoleList(bool allow_current_user_ = true, bool allow_all_ = true);

protected:
    const char * getName() const override { return "RoleList"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;

private:
    const bool allow_current_user;
    const bool allow_all;
};

}
