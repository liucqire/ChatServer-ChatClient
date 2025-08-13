#ifndef GROUP_H
#define GROUP_H

#include "groupuser.hpp"
#include <string>
#include <vector>

using namespace std;

// Group表的ORM类
class Group {
public:
    Group(int id = -1, string name = "", string desc = ""):
        id(id), name(name), desc(desc) { }

    void setId(int id) { this->id = id; }
    void setName(string name) { this->name = name; }
    void setDesc(string desc) { this->desc = desc; }

    int getId() { return id; }
    string getName() { return name; }
    string getDesc() { return desc; }
    vector<GroupUser>& getUser() { return user; }
private:
    int id;
    string name;
    string desc;
    vector<GroupUser> user; 
};

#endif