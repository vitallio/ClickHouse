#include <Parsers/ASTShowGrantsQuery.h>
#include <Parsers/ASTRolesOrUsersSet.h>
#include <Common/quoteString.h>


namespace DB
{
String ASTShowGrantsQuery::getID(char) const
{
    return "ShowGrantsQuery";
}


ASTPtr ASTShowGrantsQuery::clone() const
{
    return std::make_shared<ASTShowGrantsQuery>(*this);
}


void ASTShowGrantsQuery::formatQueryImpl(const FormatSettings & settings, FormatState &, FormatStateStacked) const
{
    settings.ostr << (settings.hilite ? hilite_keyword : "") << "SHOW GRANTS"
                  << (settings.hilite ? hilite_none : "");

    if (for_whom)
    {
        settings.ostr << (settings.hilite ? hilite_keyword : "") << " FOR "
                      << (settings.hilite ? hilite_none : "");
        for_whom->format(settings);
    }
}
}
