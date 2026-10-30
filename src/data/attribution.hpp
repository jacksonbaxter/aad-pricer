#ifndef AADP_DATA_ATTRIBUTION_HPP_
#define AADP_DATA_ATTRIBUTION_HPP_

#include <iosfwd>
#include <string_view>

namespace aadp {

// The attribution for the TAIFEX open data: the blocks for datasets 11320 and 11319 and the
// project note, as in NOTICE.
std::string_view attribution_text();

// Every writer of chain-derived output calls this. It is the only route to the text.
void write_attribution(std::ostream& out);

}  // namespace aadp

#endif  // AADP_DATA_ATTRIBUTION_HPP_
