#include <iostream>
#include <string>
#include <vector>

struct Patent {
    int id;
    char geke_code[16];
    char application_no[32];
    char title[256];
    char applicant[128];
};

class PatentManager {
public:
    void add(const Patent& p) {
        patents_.push_back(p);
    }
    
    Patent* find_by_id(int id) {
        for (auto& p : patents_) {
            if (p.id == id) return &p;
        }
        return nullptr;
    }
    
    size_t count() const {
        return patents_.size();
    }
    
private:
    std::vector<Patent> patents_;
};

int main(int argc, char* argv[]) {
    std::cout << "==================================" << std::endl;
    std::cout << "  patX v0.1.0 - Patent Manager" << std::endl;
    std::cout << "  C++/Rust High Performance" << std::endl;
    std::cout << "==================================" << std::endl;
    std::cout << std::endl;
    
    PatentManager mgr;
    
    Patent p1 = {1, "GC-0001", "CN202310000001.X", "Test Patent 1", "Company A"};
    mgr.add(p1);
    
    Patent p2 = {2, "GC-0002", "CN202310000002.X", "Test Patent 2", "Company B"};
    mgr.add(p2);
    
    std::cout << "Added " << mgr.count() << " patents." << std::endl;
    
    if (auto p = mgr.find_by_id(1)) {
        std::cout << "Found: [" << p->geke_code << "] " << p->title << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "Build successful! patX is ready." << std::endl;
    
    return 0;
}