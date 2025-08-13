#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h>

#include <vector>
#include <unordered_map>

using namespace std;
using namespace muduo;

// 获取单例对象的接口函数
ChatService* ChatService::instance() {
    static ChatService service;
    return &service;
}

ChatService::ChatService() {
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});  
    _msgHandlerMap.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, std::bind(&ChatService::addFriend, this, _1, _2, _3)});
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::loginout, this, _1, _2, _3)});

    // 连接redis服务器
    if (_redis.connect()) {
        // 设置上报消息的回调
        _redis.init_notify_handler(std::bind(&ChatService::handlerRedisSubscribeMessage, this, _1, _2));
    }
} 

MsgHandler ChatService::getHandler(int msgId) {
    if (_msgHandlerMap.count(msgId))
        return _msgHandlerMap[msgId];
    else {
        return [=](auto a, auto b, auto c) {
            LOG_ERROR << "msgId: " << msgId << "can not find handler!";
        };
    }
} 

// 处理登录业务
void ChatService::login(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int id = js["id"].get<int>();
    string pwd = js["password"];

    User user = _userModel.query(id);
    json response;

    response["id"] = id;
    response["msgid"] = LOGIN_MSG_ACK;
    if (user.getId() != -1 && user.getPwd() == pwd) {
        if (user.getState() == "online") {
            response["errno"] = 2;
            response["errmsg"] = "this account is using, input another!";
        } else {
            // 登录成功,更新用户状态信息
            user.setState("online");
            _userModel.updateState(user);

            // 记录用户连接信息
            // lock_guard 构造时加锁,析构时解锁
            {
                lock_guard<mutex> lock(_connMutex);
                _userconnMap.insert({id, conn});
            }

            // id用户登录成功后,向redis订阅channel
            _redis.subscribe(id);

            response["errno"] = 0;
            response["name"] = user.getName();

            // 查询该用户是否有离线信息
            vector<string> vec = _offlineMsgModel.query(id);
            if (vec.size()) {
                response["offlinemsg"] = vec;
                // 读取完用户的所有离线消息后, 删除所有离线消息
                _offlineMsgModel.remove(id);
            }

            // 查询用户好友信息并返回
            vector<User> userVec = _friendModel.query(id);
            if (userVec.size()) {
                vector<string> vec2;
                for (auto &user : userVec) {
                    json js;
                    js["id"] = user.getId();
                    js["name"] = user.getName();
                    js["state"] = user.getState();
                    vec2.push_back(js.dump());
                }
                response["friends"] = vec2;
            }

            // 查询用户的群组信息
            vector<Group> groupuserVec = _groupModel.queryGroup(id);
            if (groupuserVec.size()) {
                vector<string> groupV;
                for (auto &group : groupuserVec) {
                    json js;
                    js["id"] = group.getId();
                    js["groupname"] = group.getName();
                    js["groupdesc"] = group.getDesc();
                    vector<string> userV;
                    for (auto &user : group.getUser()) {
                        json js2;
                        js2["id"] = user.getId();
                        js2["name"] = user.getName();
                        js2["state"] = user.getState();
                        js2["role"] = user.getRole();
                        userV.push_back(js2.dump());
                    }
                    js["users"] = userV;
                    groupV.push_back(js.dump());
                }
                response["groups"] = groupV;
            }
        }
    } else {
        response["errno"] = 1;
        response["errmsg"] = "id or password is invalid";
    }
    conn->send(response.dump());
}

// 处理注册业务
void ChatService::reg(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    string name = js["name"];
    string pwd = js["password"];

    User user;
    user.setName(name);
    user.setPwd(pwd);
    bool state = _userModel.insert(user);
    json response;
    response["msgid"] = REG_MSG_ACK;
    response["id"] = user.getId();    
    if (state) { // 注册成功
        response["errno"] = 0;
    } else {
        response["errno"] = 1;
    }
    conn->send(response.dump());
} 

void ChatService::clientCloseException(const TcpConnectionPtr& conn) {
    User user;

    // 对_userconn表进行操作需要注意线程安全
    {
        lock_guard<mutex> lock(_connMutex);
        //从用户表删除用户的连接信息
        for (auto it = _userconnMap.begin(); it != _userconnMap.end(); it++) {
            if (it->second == conn) {
                user.setId(it->first);
                _userconnMap.erase(it);
                break;
            }
        }
    }

    // 用户注销,取消订阅通道
    _redis.unsubscribe(user.getId());

    //更新用户信息
    if (user.getId() != -1) {
        user.setState("offline");
        _userModel.updateState(user);
    }
}

// 一对一聊天业务
void ChatService::oneChat(const TcpConnectionPtr& conn, json &js, Timestamp time) {
    int toid = js["toid"].get<int>();

    {   // 在同一台服务器上
        lock_guard<mutex> lock(_connMutex);
        auto it = _userconnMap.find(toid);
        if (it != _userconnMap.end()) {
            // user 在线,转发消息
            it->second->send(js.dump());
            return;
        }
    }

    // 查询是否在其他服务器上在线
    User user = _userModel.query(toid);
    if (user.getState() == "online") {
        _redis.publish(toid, js.dump());
        return;
    }

    // user 不在线,存储离线消息
    _offlineMsgModel.insert(toid, js.dump());
}

// 服务器异常处理方法
void ChatService::reset() {
    // 将online状态的用户,重置成offline
    _userModel.resetState();
}

// 添加好友业务
void ChatService::addFriend(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    // 存储好友信息
    _friendModel.insert(userid, friendid);
}

// 创建群组业务
void ChatService::createGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    string name = js["groupname"];
    string desc = js["groupdesc"];

    //存储新建群组的信息
    Group group(-1, name, desc);
    if (_groupModel.createGroup(group)) {
        // 存储群组创建人信息
        _groupModel.addGroup(userid, group.getId(), "creator");
    }
}

// 加入群组业务
void ChatService::addGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    _groupModel.addGroup(userid, groupid, "normal");

}

// 群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    vector<int> idVec = _groupModel.queryGroupUsers(userid, groupid);
    lock_guard<mutex> lock(_connMutex);
    for (int id : idVec) {
        auto it = _userconnMap.find(id);
        if (it != _userconnMap.end()) {
            // 在同一台服务器上,转发群消息
            it->second->send(js.dump());
        } else {
            User user = _userModel.query(id);

            // 查询toid是否在其他服务器上在线
            if (user.getState() == "online") {
                _redis.publish(id, js.dump());
            } else {
                // 存储离线消息
                _offlineMsgModel.insert(id, js.dump());
            }
        }
    }
}

void ChatService::loginout(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int id = js["id"].get<int>();
     
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userconnMap.find(id);
        if (it != _userconnMap.end()) {
            _userconnMap.erase(it);
        }
    }

    // 用户注销,相当于下线,取消订阅通道
    _redis.unsubscribe(id);

    // 更新用户状态信息
    User user;
    user.setId(id);
    _userModel.updateState(user);
}

// 从redis消息队列中获取订阅的消息
void ChatService::handlerRedisSubscribeMessage(int userid, string msg) {
    lock_guard<mutex> lock(_connMutex);
    auto it = _userconnMap.find(userid);
    if (it != _userconnMap.end()) {
        it->second->send(msg);
        return;
    }

    // 存储该用户的离线消息
    _offlineMsgModel.insert(userid, msg);
}