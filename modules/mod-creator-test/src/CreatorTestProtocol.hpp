#pragma once
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>

namespace CreatorTest {
// Shared, versioned local file protocol. No character database writes or server commands.
struct Request {
    std::string token;
    uint32_t character = 0, account = 0, map = 0;
    double x = 0, y = 0, z = 0, orientation = 0;
    int64_t expires = 0;
};
inline bool valid(Request const& r, int64_t now) {
    if (r.token.size()!=32 || !r.character || !r.account || r.map>65535) return false;
    for(char c:r.token) if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    return r.expires>now && r.expires<=now+1800 && std::isfinite(r.x) && std::isfinite(r.y)
        && std::isfinite(r.z) && std::isfinite(r.orientation)
        && std::abs(r.x)<17066 && std::abs(r.y)<17066 && std::abs(r.z)<100000
        && r.orientation>=0 && r.orientation<6.283186;
}
inline bool matches(Request const& r, uint32_t character, uint32_t account, int64_t now) {
    return valid(r,now)&&r.character==character&&r.account==account;
}
inline bool localOnly(bool enable,std::string const& bind,std::string const& world,std::string const& characters,std::string const& login) {
    return enable && bind=="127.0.0.1" && world.rfind("127.0.0.1;",0)==0
        && characters.rfind("127.0.0.1;",0)==0 && login.rfind("127.0.0.1;",0)==0;
}
inline bool read(std::istream& in, Request& r, int64_t now) {
    unsigned version=0;
    if(!(in>>version>>r.token>>r.character>>r.account>>r.map>>r.x>>r.y>>r.z>>r.orientation>>r.expires)) return false;
    std::string extra;
    return version==1 && !(in>>extra) && valid(r,now);
}
inline void write(std::ostream& out, Request const& r) {
    out<<std::setprecision(17)<<"1\n"<<r.token<<'\n'<<r.character<<' '<<r.account<<'\n'
       <<r.map<<' '<<r.x<<' '<<r.y<<' '<<r.z<<' '<<r.orientation<<'\n'<<r.expires<<'\n';
}
}
