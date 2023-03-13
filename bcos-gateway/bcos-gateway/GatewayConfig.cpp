/** @file GatewayConfig.cpp
 *  @author octopus
 *  @date 2021-05-19
 */

#include "bcos-gateway/Common.h"
#include "bcos-utilities/BoostLog.h"
#include <bcos-framework/protocol/Protocol.h>
#include <bcos-gateway/GatewayConfig.h>
#include <bcos-security/bcos-security/KeyCenter.h>
#include <bcos-utilities/DataConvertUtility.h>
#include <bcos-utilities/FileUtility.h>
#include <bcos-utilities/FixedBytes.h>
#include <json/json.h>
#include <boost/throw_exception.hpp>
#include <algorithm>
#include <limits>
#include <string>

using namespace bcos;
using namespace security;
using namespace gateway;

bool GatewayConfig::isValidPort(int port)
{
    return port > 1024 && port <= 65535;
}

// check if the ip valid
bool GatewayConfig::isValidIP(const std::string& _ip)
{
    boost::system::error_code ec;
    boost::asio::ip::address ip_address = boost::asio::ip::make_address(_ip, ec);
    std::ignore = ip_address;
    return ec.value() == 0;
}

// MB to bit
int64_t GatewayConfig::doubleMBToBit(double _d)
{
    _d *= (1024 * 1024 / 8);

    return (int64_t)_d;
}

void GatewayConfig::hostAndPort2Endpoint(const std::string& _host, NodeIPEndpoint& _endpoint)
{
    std::string ip;
    uint16_t port;

    std::vector<std::string> s;
    boost::split(s, _host, boost::is_any_of("]"), boost::token_compress_on);
    if (s.size() == 2)
    {  // ipv6
        ip = s[0].data() + 1;
        port = boost::lexical_cast<int>(s[1].data() + 1);
    }
    else if (s.size() == 1)
    {  // ipv4
        std::vector<std::string> v;
        boost::split(v, _host, boost::is_any_of(":"), boost::token_compress_on);
        if (v.size() < 2)
        {
            BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                      "GatewayConfig: invalid host , host=" + _host));
        }
        ip = v[0];
        port = boost::lexical_cast<int>(v[1]);
    }
    else
    {
        GATEWAY_CONFIG_LOG(ERROR) << LOG_DESC("not valid host value") << LOG_KV("host", _host);
        BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                  "GatewayConfig: the host is invalid, host=" + _host));
    }

    if (!isValidPort(port))
    {
        GATEWAY_CONFIG_LOG(ERROR) << LOG_DESC("the port is not valid") << LOG_KV("port", port);
        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment(
                "GatewayConfig: the port is invalid, port=" + std::to_string(port)));
    }

    boost::system::error_code ec;
    boost::asio::ip::address ip_address = boost::asio::ip::make_address(ip, ec);
    if (ec.value() != 0)
    {
        GATEWAY_CONFIG_LOG(ERROR) << LOG_DESC("the host is invalid, make_address error")
                                  << LOG_KV("host", _host);
        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment(
                "GatewayConfig: the host is invalid make_address error, host=" + _host));
    }

    _endpoint = NodeIPEndpoint{ip_address, port};
}

void GatewayConfig::parseConnectedJson(
    const std::string& _json, std::set<NodeIPEndpoint>& _nodeIPEndpointSet)
{
    /*
    {"nodes":["127.0.0.1:30355","127.0.0.1:30356"}]}
    */
    Json::Value root;
    Json::Reader jsonReader;

    try
    {
        if (!jsonReader.parse(_json, root))
        {
            GATEWAY_CONFIG_LOG(ERROR)
                << "unable to parse connected nodes json" << LOG_KV("json:", _json);
            BOOST_THROW_EXCEPTION(
                InvalidParameter() << errinfo_comment("GatewayConfig: unable to parse p2p "
                                                      "connected nodes json"));
        }

        std::set<NodeIPEndpoint> nodeIPEndpointSet;
        Json::Value jNodes = root["nodes"];
        if (jNodes.isArray())
        {
            unsigned int jNodesSize = jNodes.size();
            for (unsigned int i = 0; i < jNodesSize; i++)
            {
                std::string host = jNodes[i].asString();

                NodeIPEndpoint endpoint;
                hostAndPort2Endpoint(host, endpoint);
                _nodeIPEndpointSet.insert(endpoint);

                GATEWAY_CONFIG_LOG(INFO)
                    << LOG_DESC("add one connected node") << LOG_KV("host", host);
            }
        }
    }
    catch (const std::exception& e)
    {
        GATEWAY_CONFIG_LOG(ERROR) << LOG_KV(
            "parseConnectedJson error: ", boost::diagnostic_information(e));
        BOOST_THROW_EXCEPTION(e);
    }
}

/**
 * @brief: loads configuration items from the config.ini
 * @param _configPath: config.ini path
 * @return void
 */
void GatewayConfig::initConfig(std::string const& _configPath, bool _uuidRequired)
{
    try
    {
        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(_configPath, pt);
        initP2PConfig(pt, _uuidRequired);
        initPeerBlacklistConfig(pt);
        initPeerWhitelistConfig(pt);
        initRateLimitConfig(pt);
        if (m_smSSL)
        {
            initSMCertConfig(pt);
        }
        else
        {
            initCertConfig(pt);
        }
    }
    catch (const std::exception& e)
    {
        boost::filesystem::path full_path(boost::filesystem::current_path());

        GATEWAY_CONFIG_LOG(ERROR) << LOG_KV("configPath", _configPath)
                                  << LOG_KV("currentPath", full_path.string())
                                  << LOG_KV("initConfig error: ", boost::diagnostic_information(e));

        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment("initConfig: currentPath:" + full_path.string() +
                                                  " ,error:" + boost::diagnostic_information(e)));
    }

    GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("initConfig ok!") << LOG_KV("configPath", _configPath)
                             << LOG_KV("listenIP", m_listenIP) << LOG_KV("listenPort", m_listenPort)
                             << LOG_KV("smSSL", m_smSSL)
                             << LOG_KV("peers", m_connectedNodes.size());
}

/// loads p2p configuration items from the configuration file
void GatewayConfig::initP2PConfig(const boost::property_tree::ptree& _pt, bool _uuidRequired)
{
    /*
    [p2p]
      ; uuid
      uuid =
      ; ssl or sm ssl
      sm_ssl=true
      ;
      enable_rip_protocol=false
      listen_ip=0.0.0.0
      listen_port=30300
      nodes_path=./
      nodes_file=nodes.json

      enable_rip_protocol=true
      allow_max_msg_size=
      session_recv_buffer_size=
      session_max_read_data_size=
      session_max_send_data_size=
      session_max_send_msg_count=
      */
    m_uuid = _pt.get<std::string>("p2p.uuid", "");
    if (_uuidRequired && m_uuid.empty())
    {
        BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                  "initP2PConfig: invalid uuid! Must be non-empty!"));
    }
    bool smSSL = _pt.get<bool>("p2p.sm_ssl", false);
    std::string listenIP = _pt.get<std::string>("p2p.listen_ip", "0.0.0.0");
    int listenPort = _pt.get<int>("p2p.listen_port", 30300);
    if (!isValidPort(listenPort))
    {
        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment(
                "initP2PConfig: invalid listen port, port=" + std::to_string(listenPort)));
    }

    // not set the nodePath, load from the config
    if (m_nodePath.empty())
    {
        m_nodePath = _pt.get<std::string>("p2p.nodes_path", "./");
    }

    m_nodeFileName = _pt.get<std::string>("p2p.nodes_file", "nodes.json");

    m_enableRIPProtocol = _pt.get<bool>("p2p.enable_rip_protocol", true);

    constexpr static uint32_t defaultAllowMaxMsgSize = 32 * 1024 * 1024;
    m_allowMaxMsgSize = _pt.get<uint32_t>("p2p.allow_max_msg_size", defaultAllowMaxMsgSize);

    uint32_t defaultRecvBufferSize = 2 * m_allowMaxMsgSize;
    m_sessionRecvBufferSize =
        _pt.get<uint32_t>("p2p.session_recv_buffer_size", defaultRecvBufferSize);

    if (m_sessionRecvBufferSize < 2 * m_allowMaxMsgSize)
    {
        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment("initP2PConfig: invalid p2p.allow_max_msg_size "
                                                  "and p2p.session_recv_buffer_size config items, "
                                                  "p2p.session_recv_buffer_size must greater equal"
                                                  "than 2 * p2p.allow_max_msg_size"));
    }

    constexpr static uint32_t defaultMaxReadDataSize = 40 * 1024;
    m_maxReadDataSize = _pt.get<uint32_t>("p2p.session_max_read_data_size", defaultMaxReadDataSize);

    constexpr static uint32_t defaultMaxSendDataSize = 1024 * 1024;
    m_maxSendDataSize = _pt.get<uint32_t>("p2p.session_max_send_data_size", defaultMaxSendDataSize);

    constexpr static uint32_t defaultMaxSendMsgCount = 10;
    m_maxSendMsgCount = _pt.get<uint32_t>("p2p.session_max_send_msg_count", defaultMaxSendMsgCount);

    m_smSSL = smSSL;
    m_listenIP = listenIP;
    m_listenPort = (uint16_t)listenPort;

    GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("initP2PConfig ok!") << LOG_KV("p2p.listen_ip", listenIP)
                             << LOG_KV("p2p.listen_port", listenPort) << LOG_KV("p2p.sm_ssl", smSSL)
                             << LOG_KV("p2p.enable_rip_protocol", m_enableRIPProtocol)
                             << LOG_KV("p2p.allow_max_msg_size", m_allowMaxMsgSize)
                             << LOG_KV("p2p.session_recv_buffer_size", m_sessionRecvBufferSize)
                             << LOG_KV("p2p.session_max_read_data_size", m_maxReadDataSize)
                             << LOG_KV("p2p.session_max_send_data_size", m_maxSendDataSize)
                             << LOG_KV("p2p.session_max_send_msg_count", m_maxSendMsgCount)
                             << LOG_KV("p2p.nodes_path", m_nodePath)
                             << LOG_KV("p2p.nodes_file", m_nodeFileName);
}

// load p2p connected peers
void GatewayConfig::loadP2pConnectedNodes()
{
    std::string nodeFilePath = m_nodePath + "/" + m_nodeFileName;
    // load p2p connected nodes
    std::set<NodeIPEndpoint> nodes;
    auto jsonContent = readContentsToString(boost::filesystem::path(nodeFilePath));
    if (!jsonContent || jsonContent->empty())
    {
        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment(
                "initP2PConfig: unable to read nodes json file, path=" + nodeFilePath));
    }

    parseConnectedJson(*jsonContent, nodes);
    m_connectedNodes = nodes;

    GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("loadP2pConnectedNodes ok!")
                             << LOG_KV("nodePath", m_nodePath)
                             << LOG_KV("nodeFileName", m_nodeFileName)
                             << LOG_KV("nodes", nodes.size());
}

/// loads ca configuration items from the configuration file
void GatewayConfig::initCertConfig(const boost::property_tree::ptree& _pt)
{
    /*
    [cert]
      ; directory the certificates located in
      ca_path=./
      ; the ca certificate file
      ca_cert=ca.crt
      ; the node private key file
      node_key=ssl.key
      ; the node certificate file
      node_cert=ssl.crt
    */
    if (m_certPath.size() == 0)
    {
        m_certPath = _pt.get<std::string>("cert.ca_path", "./");
    }
    std::string caCertFile = m_certPath + "/" + _pt.get<std::string>("cert.ca_cert", "ca.crt");
    std::string nodeCertFile = m_certPath + "/" + _pt.get<std::string>("cert.node_cert", "ssl.crt");
    std::string nodeKeyFile = m_certPath + "/" + _pt.get<std::string>("cert.node_key", "ssl.key");
    std::string multiCaPath =
        m_certPath + "/" + _pt.get<std::string>("cert.multi_ca_path", "multiCaPath");

    GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("initCertConfig") << LOG_KV("ca_path", m_certPath)
                             << LOG_KV("ca_cert", caCertFile) << LOG_KV("node_cert", nodeCertFile)
                             << LOG_KV("node_key", nodeKeyFile)
                             << LOG_KV("mul_ca_path", multiCaPath);

    checkFileExist(caCertFile);
    checkFileExist(nodeCertFile);
    checkFileExist(nodeKeyFile);

    CertConfig certConfig;
    certConfig.caCert = caCertFile;
    certConfig.nodeCert = nodeCertFile;
    certConfig.nodeKey = nodeKeyFile;
    certConfig.multiCaPath = multiCaPath;

    m_certConfig = certConfig;

    GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("initCertConfig") << LOG_KV("ca", certConfig.caCert)
                             << LOG_KV("node_cert", certConfig.nodeCert)
                             << LOG_KV("node_key", certConfig.nodeKey)
                             << LOG_KV("mul_ca_path", certConfig.multiCaPath);
}

// loads sm ca configuration items from the configuration file
void GatewayConfig::initSMCertConfig(const boost::property_tree::ptree& _pt)
{
    /*
    [cert]
    ; directory the certificates located in
    ca_path=./
    ; the ca certificate file
    sm_ca_cert=sm_ca.crt
    ; the node private key file
    sm_node_key=sm_ssl.key
    ; the node certificate file
    sm_node_cert=sm_ssl.crt
    ; the node private key file
    sm_ennode_key=sm_enssl.key
    ; the node certificate file
    sm_ennode_cert=sm_enssl.crt
    */
    // not set the certPath, load from the configuration
    if (m_certPath.size() == 0)
    {
        m_certPath = _pt.get<std::string>("cert.ca_path", "./");
    }
    std::string smCaCertFile =
        m_certPath + "/" + _pt.get<std::string>("cert.sm_ca_cert", "sm_ca.crt");
    std::string smNodeCertFile =
        m_certPath + "/" + _pt.get<std::string>("cert.sm_node_cert", "sm_ssl.crt");
    std::string smNodeKeyFile =
        m_certPath + "/" + _pt.get<std::string>("cert.sm_node_key", "sm_ssl.key");
    std::string smEnNodeCertFile =
        m_certPath + "/" + _pt.get<std::string>("cert.sm_ennode_cert", "sm_enssl.crt");
    std::string smEnNodeKeyFile =
        m_certPath + "/" + _pt.get<std::string>("cert.sm_ennode_key", "sm_enssl.key");
    std::string multiCaPath =
        m_certPath + "/" + _pt.get<std::string>("cert.multi_ca_path", "multiCaPath");

    checkFileExist(smCaCertFile);
    checkFileExist(smNodeCertFile);
    checkFileExist(smNodeKeyFile);
    checkFileExist(smEnNodeCertFile);
    checkFileExist(smEnNodeKeyFile);

    SMCertConfig smCertConfig;
    smCertConfig.caCert = smCaCertFile;
    smCertConfig.nodeCert = smNodeCertFile;
    smCertConfig.nodeKey = smNodeKeyFile;
    smCertConfig.enNodeCert = smEnNodeCertFile;
    smCertConfig.enNodeKey = smEnNodeKeyFile;
    smCertConfig.multiCaPath = multiCaPath;

    m_smCertConfig = smCertConfig;

    GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("initSMCertConfig") << LOG_KV("ca_path", m_certPath)
                             << LOG_KV("sm_ca_cert", smCertConfig.caCert)
                             << LOG_KV("sm_node_cert", smCertConfig.nodeCert)
                             << LOG_KV("sm_node_key", smCertConfig.nodeKey)
                             << LOG_KV("sm_ennode_cert", smCertConfig.enNodeCert)
                             << LOG_KV("sm_ennode_key", smCertConfig.enNodeKey)
                             << LOG_KV("multi_ca_path", smCertConfig.multiCaPath);
}

// loads rate limit configuration items from the configuration file
void GatewayConfig::initRateLimitConfig(const boost::property_tree::ptree& _pt)
{
    /*
    [flow_control]
    ; time window for ratelimiter
    time_window_sec=3
    ;
    ; distributed rate limit switch
    enable_distributed_ratelimit=true
    enable_distributed_ratelimit_cache=true
    distributed_ratelimit_cache_percent=15
    ;
    ; rate limiter stat reporter interval, unit: ms
    stat_reporter_interval=60000
     */
    // time_window_sec=1
    int32_t timeWindowSec = _pt.get<int32_t>("flow_control.time_window_sec", 1);

    // enable_distributed_ratelimit=false
    bool enableDistributedRatelimit =
        _pt.get<bool>("flow_control.enable_distributed_ratelimit", false);
    // enable_distributed_ratelimit=false
    bool enableDistributedRateLimitCache =
        _pt.get<bool>("flow_control.enable_distributed_ratelimit_cache", true);
    // enable_distributed_ratelimit=false
    int32_t distributedRateLimitCachePercent =
        _pt.get<int32_t>("flow_control.distributed_ratelimit_cache_percent", 20);
    // stat_reporter_interval=60000
    int32_t statInterval = _pt.get<int32_t>("flow_control.stat_reporter_interval", 60000);

    GATEWAY_CONFIG_LOG(INFO) << LOG_BADGE("initRateLimiterConfig")
                             << LOG_DESC("the rate limit general config")
                             << LOG_KV("stat_reporter_interval", statInterval)
                             << LOG_KV("time_window_sec", timeWindowSec)
                             << LOG_KV("enable_distributed_ratelimit", enableDistributedRatelimit)
                             << LOG_KV("enable_distributed_ratelimit_cache",
                                    enableDistributedRateLimitCache)
                             << LOG_KV("distributed_ratelimit_cache_percent",
                                    distributedRateLimitCachePercent);

    // --------------------------------- outgoing begin -------------------------------------------

    /*
    [flow_control]
    ; the module that does not limit bandwidth
    ; list of all modules: raft,pbft,amop,block_sync,txs_sync,light_node,cons_txs_sync
    ;
    modules_without_bw_limit=raft,pbft

    outgoing_allow_exceed_max_permit=false
    ;
    ; restrict the outgoing bandwidth of the node
    ; both integer and decimal is support, unit: Mb
    ;
    total_outgoing_bw_limit=10

    ; restrict the outgoing bandwidth of the the connection
    ; both integer and decimal is support, unit: Mb
    ;
    conn_outgoing_bw_limit=2
    ;
    ; specify IP to limit bandwidth, format: conn_outgoing_bw_limit_x.x.x.x=n
        conn_outgoing_bw_limit_192.108.0.1=3
        conn_outgoing_bw_limit_192.108.0.2=3
        conn_outgoing_bw_limit_192.108.0.3=3
    ;
    ; default bandwidth limit for the group
    group_outgoing_bw_limit=2
    ;
    ; specify group to limit bandwidth, group_outgoing_bw_limit_groupName=n
        group_outgoing_bw_limit_group0=2
        group_outgoing_bw_limit_group1=2
        group_outgoing_bw_limit_group2=2
    */

    // outgoing_allow_exceed_max_permit
    bool allowExceedMaxPermitSize =
        _pt.get<bool>("flow_control.outgoing_allow_exceed_max_permit", false);

    // modules_without_bw_limit=raft,pbft
    std::string strModulesWithoutLimit =
        _pt.get<std::string>("flow_control.modules_without_bw_limit", "raft,pbft,cons_txs_sync");

    std::set<uint16_t> moduleIDs;
    std::vector<std::string> modules;

    if (!strModulesWithoutLimit.empty())
    {
        boost::split(
            modules, strModulesWithoutLimit, boost::is_any_of(","), boost::token_compress_on);

        for (auto& module : modules)
        {
            boost::trim(module);
            boost::algorithm::to_lower(module);
            auto optModuleID = protocol::stringToModuleID(module);
            // TODO: modify module name to module id
            if (!optModuleID.has_value())
            {
                BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                          "unrecognized module: " + module +
                                          " ,list of available modules: "
                                          "raft,pbft,amop,block_sync,txs_sync,light_node"));
            }
            moduleIDs.insert(optModuleID.value());
        }
    }

    int64_t totalOutgoingBwLimit = -1;
    // total_outgoing_bw_limit
    std::string value = _pt.get<std::string>("flow_control.total_outgoing_bw_limit", "");
    if (value.empty())
    {
        GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("the total_outgoing_bw_limit is not initialized");
    }
    else
    {
        auto bandwidth = boost::lexical_cast<double>(value);
        totalOutgoingBwLimit = doubleMBToBit(bandwidth);

        GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("the total_outgoing_bw_limit has been initialized")
                                 << LOG_KV("value", value) << LOG_KV("bandwidth", bandwidth)
                                 << LOG_KV("totalOutgoingBwLimit", totalOutgoingBwLimit);
    }

    int64_t connOutgoingBwLimit = -1;
    // conn_outgoing_bw_limit
    value = _pt.get<std::string>("flow_control.conn_outgoing_bw_limit", "");
    if (value.empty())
    {
        GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("the conn_outgoing_bw_limit is not initialized");
    }
    else
    {
        auto bandwidth = boost::lexical_cast<double>(value);
        connOutgoingBwLimit = doubleMBToBit(bandwidth);

        GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("the conn_outgoing_bw_limit has been initialized")
                                 << LOG_KV("value", value) << LOG_KV("bandwidth", bandwidth)
                                 << LOG_KV("connOutgoingBwLimit", connOutgoingBwLimit);
    }

    int64_t groupOutgoingBwLimit = -1;
    // group_outgoing_bw_limit
    value = _pt.get<std::string>("flow_control.group_outgoing_bw_limit", "");
    if (value.empty())
    {
        GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("the group_outgoing_bw_limit is not initialized");
    }
    else
    {
        auto bandwidth = boost::lexical_cast<double>(value);
        groupOutgoingBwLimit = doubleMBToBit(bandwidth);

        GATEWAY_CONFIG_LOG(INFO) << LOG_DESC("the group_outgoing_bw_limit has been initialized")
                                 << LOG_KV("value", value) << LOG_KV("bandwidth", bandwidth)
                                 << LOG_KV("groupOutgoingBwLimit", groupOutgoingBwLimit);
    }

    // ip => bandwidth && group => bandwidth
    if (_pt.get_child_optional("flow_control"))
    {
        for (auto const& it : _pt.get_child("flow_control"))
        {
            auto key = it.first;
            auto value = it.second.data();

            boost::trim(key);
            boost::trim(value);

            if (boost::starts_with(key, "conn_outgoing_bw_limit_"))
            {
                // ip_outgoing_bw_x.x.x.x =
                std::string ip = key.substr(strlen("conn_outgoing_bw_limit_"));
                if (!isValidIP(ip))
                {
                    BOOST_THROW_EXCEPTION(
                        InvalidParameter() << errinfo_comment(
                            "flow_control.ip_outgoing_bw_x.x.x.x config, invalid ip format, ip: " +
                            ip));
                }
                auto bandwidth = doubleMBToBit(boost::lexical_cast<double>(value));
                if (bandwidth > 0)
                {
                    m_rateLimiterConfig.ip2BwLimit[ip] = bandwidth;
                }

                GATEWAY_CONFIG_LOG(INFO)
                    << LOG_BADGE("initRateLimiterConfig") << LOG_DESC("add ip bandwidth limit")
                    << LOG_KV("ip", ip) << LOG_KV("bandwidth", bandwidth);
            }  // group_outgoing_bw_group0
            else if (boost::starts_with(key, "group_outgoing_bw_limit_"))
            {
                // group_xxxx =
                std::string group = key.substr(strlen("group_outgoing_bw_limit_"));
                auto bandwidth = doubleMBToBit(boost::lexical_cast<double>(value));
                if (bandwidth > 0)
                {
                    m_rateLimiterConfig.group2BwLimit[group] = bandwidth;
                }

                GATEWAY_CONFIG_LOG(INFO)
                    << LOG_BADGE("initRateLimiterConfig") << LOG_DESC("add group bandwidth limit")
                    << LOG_KV("group", group) << LOG_KV("bandwidth", bandwidth);
            }
        }
    }

    GATEWAY_CONFIG_LOG(INFO)
        << LOG_BADGE("initRateLimiterConfig") << LOG_DESC("the outgoing bandwidth rate limit")
        << LOG_KV("outgoing_allow_exceed_max_permit", allowExceedMaxPermitSize)
        << LOG_KV("total_outgoing_bw_limit", totalOutgoingBwLimit)
        << LOG_KV("conn_outgoing_bw_limit", connOutgoingBwLimit)
        << LOG_KV("group_outgoing_bw_limit", groupOutgoingBwLimit)
        << LOG_KV("conn_outgoing_bw_limit_x.x.x.x", m_rateLimiterConfig.ip2BwLimit.size())
        << LOG_KV("group_outgoing_bw_limit_xx", m_rateLimiterConfig.group2BwLimit.size())
        << LOG_KV("modules_without_bw_limit size", m_rateLimiterConfig.modulesWithoutLimit.size());

    // --------------------------------- outgoing end -------------------------------------------

    // --------------------------------- incoming begin -----------------------------------------

    /*
   [flow_control]
   ; incoming_p2p_basic_msg_type_list=
   ; incoming_p2p_basic_msg_type_qps_limit=-1
   ; incoming_module_msg_type_qps_limit=-1
   ;       incoming_module_qps_limit_xxxx=1000
   ;       incoming_module_qps_limit_xxxx=2000
   ;       incoming_module_qps_limit_xxxx=3000
   */
    // incoming_p2p_basic_msg_type_list=1,2,3
    std::string strP2pBasicMsgTypeList =
        _pt.get<std::string>("flow_control.incoming_p2p_basic_msg_type_list", "");
    std::set<uint16_t> p2pBasicMsgTypeList;
    if (!strP2pBasicMsgTypeList.empty())
    {
        std::vector<std::string> vMsgType;
        boost::split(
            vMsgType, strP2pBasicMsgTypeList, boost::is_any_of(","), boost::token_compress_on);
        for (auto& strMsg : vMsgType)
        {
            boost::trim(strMsg);
            if (strMsg.empty())
            {
                continue;
            }
            auto i = std::stoi(strMsg);
            if (i < 0 && i > std::numeric_limits<uint16_t>::max())
            {
                BOOST_THROW_EXCEPTION(
                    InvalidParameter() << errinfo_comment("flow_control.incoming_p2p_basic_msg_"
                                                          "type_list contains invalid module id, "
                                                          "module id should be in range (0, 255]"));
            }
            p2pBasicMsgTypeList.insert(i);
        }
    }

    // incoming_p2p_basic_msg_type_qps_limit = -1
    int32_t p2pBasicMsgQPS =
        _pt.get<int32_t>("flow_control.incoming_p2p_basic_msg_type_qps_limit", -1);
    // incoming_module_msg_type_qps_limit = -1
    int32_t moduleMsgQPS = _pt.get<int32_t>("flow_control.incoming_module_msg_type_qps_limit", -1);
    // module id => qps
    if (_pt.get_child_optional("flow_control"))
    {
        for (auto const& it : _pt.get_child("flow_control"))
        {
            auto key = it.first;
            auto value = it.second.data();

            boost::trim(key);
            boost::trim(value);
            if (boost::starts_with(key, "incoming_module_qps_limit_"))
            {
                // incoming_module_qps_limit_xxxx =
                std::string strModule = key.substr(strlen("incoming_module_qps_limit_"));
                auto module = boost::lexical_cast<int>(strModule);
                auto qps = boost::lexical_cast<int>(value);
                if (qps > 0)
                {
                    m_rateLimiterConfig.moduleMsg2QPS[module] = qps;

                    GATEWAY_CONFIG_LOG(INFO)
                        << LOG_BADGE("initRateLimiterConfig") << LOG_DESC("add module qps limit")
                        << LOG_KV("module", module) << LOG_KV("qps", qps);
                }
            }
        }
    }

    GATEWAY_CONFIG_LOG(INFO) << LOG_BADGE("initRateLimiterConfig")
                             << LOG_DESC("the incoming qps rate limit")
                             << LOG_KV("incoming_p2p_basic_msg_type_qps_limit", p2pBasicMsgQPS)
                             << LOG_KV("incoming_module_msg_type_qps_limit", p2pBasicMsgQPS)
                             << LOG_KV("incoming_module_qps_limit_xxx size",
                                    m_rateLimiterConfig.moduleMsg2QPS.size());

    // --------------------------------- incoming end -------------------------------------------

    m_rateLimiterConfig.timeWindowSec = timeWindowSec;
    m_rateLimiterConfig.allowExceedMaxPermitSize = allowExceedMaxPermitSize;
    m_rateLimiterConfig.statInterval = statInterval;
    m_rateLimiterConfig.modulesWithoutLimit = moduleIDs;
    m_rateLimiterConfig.totalOutgoingBwLimit = totalOutgoingBwLimit;
    m_rateLimiterConfig.connOutgoingBwLimit = connOutgoingBwLimit;
    m_rateLimiterConfig.groupOutgoingBwLimit = groupOutgoingBwLimit;
    m_rateLimiterConfig.enableDistributedRatelimit = enableDistributedRatelimit;
    m_rateLimiterConfig.enableDistributedRateLimitCache = enableDistributedRateLimitCache;
    m_rateLimiterConfig.distributedRateLimitCachePercent = distributedRateLimitCachePercent;

    m_rateLimiterConfig.p2pBasicMsgQPS = p2pBasicMsgQPS;
    m_rateLimiterConfig.p2pModuleMsgQPS = moduleMsgQPS;
    if (!p2pBasicMsgTypeList.empty())
    {
        m_rateLimiterConfig.p2pBasicMsgTypes = p2pBasicMsgTypeList;
    }

    if (totalOutgoingBwLimit > 0 && connOutgoingBwLimit > 0 &&
        connOutgoingBwLimit > totalOutgoingBwLimit)
    {
        BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                  "flow_control.conn_outgoing_bw_limit should not greater "
                                  "than flow_control.total_outgoing_bw_limit"));
    }

    if (totalOutgoingBwLimit > 0 && groupOutgoingBwLimit > 0 &&
        groupOutgoingBwLimit > totalOutgoingBwLimit)
    {
        BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                  "flow_control.group_outgoing_bw_limit should not greater "
                                  "than flow_control.total_outgoing_bw_limit"));
    }

    if (m_rateLimiterConfig.enableDistributedRatelimit)
    {
        GATEWAY_CONFIG_LOG(INFO)
            << LOG_BADGE("initRateLimiterConfig")
            << LOG_DESC("allow distributed ratelimit, load the redis configurations");

        initRedisConfig(_pt);
    }
}

// loads redis config
void GatewayConfig::initRedisConfig(const boost::property_tree::ptree& _pt)
{
    /*
    [redis]
        server_ip=
        server_port=
        request_timeout=
        connection_pool_size=
        password=
        db=
     */

    // server_ip
    std::string redisServerIP = _pt.get<std::string>("redis.server_ip", "");
    if (redisServerIP.empty())
    {
        BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                  "initRedisConfig: invalid redis.server_ip! Must be non-empty!"));
    }

    if (!isValidIP(redisServerIP))
    {
        BOOST_THROW_EXCEPTION(InvalidParameter() << errinfo_comment(
                                  "initRedisConfig: invalid redis.server_ip! Invalid ip format!"));
    }

    // server_port
    uint16_t redisServerPort = _pt.get<uint16_t>("redis.server_port", 0);
    if (!isValidPort(redisServerPort))
    {
        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment("initRedisConfig: invalid redis.server_port! "
                                                  "redis port must be in range (1024,65535]!"));
    }

    // request_timeout
    int32_t redisTimeout = _pt.get<int32_t>("redis.request_timeout", -1);

    // connection_pool_size
    int32_t redisPoolSize = _pt.get<int32_t>("redis.connection_pool_size", 16);

    // password
    std::string redisPassword = _pt.get<std::string>("redis.password", "");

    // db
    int redisDB = _pt.get<int>("redis.db", 0);

    m_redisConfig.host = redisServerIP;
    m_redisConfig.port = redisServerPort;
    m_redisConfig.timeout = redisTimeout;
    m_redisConfig.connectionPoolSize = redisPoolSize;
    m_redisConfig.password = redisPassword;
    m_redisConfig.db = redisDB;

    GATEWAY_CONFIG_LOG(INFO) << LOG_BADGE("initRedisConfig")
                             << LOG_KV("redisServerIP", redisServerIP)
                             << LOG_KV("redisServerPort", redisServerPort)
                             << LOG_KV("redisDB", redisDB) << LOG_KV("redisTimeout", redisTimeout)
                             << LOG_KV("redisPoolSize", redisPoolSize)
                             << LOG_KV("redisPassword", redisPassword);
}

void GatewayConfig::initPeerBlacklistConfig(const boost::property_tree::ptree& _pt)
{
    std::string certBlacklistSection{"crl"};
    if (_pt.get_child_optional("certificate_blacklist"))
    {
        certBlacklistSection = "certificate_blacklist";
    }

    bool enableBlacklist{false};
    // CRL means certificate rejected list, CRL optional in config.ini
    if (_pt.get_child_optional(certBlacklistSection))
    {
        for (auto it : _pt.get_child(certBlacklistSection))
        {
            if (0 == it.first.find("crl."))
            {
                try
                {
                    std::string nodeID{boost::to_upper_copy(it.second.data())};
                    GATEWAY_CONFIG_LOG(TRACE) << LOG_BADGE("GatewayConfig")
                                              << LOG_DESC("get certificate rejected by nodeID")
                                              << LOG_KV("nodeID", nodeID);
                    bool isNodeIDValid =
                        (false == m_smSSL ? isNodeIDOk<h2048>(nodeID) : isNodeIDOk<h512>(nodeID));
                    if (true == isNodeIDValid)
                    {
                        m_enableBlacklist = true;
                        m_certBlacklist.emplace(std::move(nodeID));
                    }
                    else
                    {
                        GATEWAY_CONFIG_LOG(ERROR)
                            << LOG_BADGE("GatewayConfig")
                            << LOG_DESC("get certificate accepted by nodeID failed, illegal nodeID")
                            << LOG_KV("nodeID", nodeID);
                    }
                }
                catch (std::exception& e)
                {
                    GATEWAY_CONFIG_LOG(ERROR)
                        << LOG_BADGE("GatewayConfig") << LOG_DESC("get certificate rejected failed")
                        << LOG_KV("EINFO", boost::diagnostic_information(e));
                }
            }
        }
    }
}

void GatewayConfig::initPeerWhitelistConfig(const boost::property_tree::ptree& _pt)
{
    std::string certWhitelistSection{"cal"};
    if (_pt.get_child_optional("certificate_whitelist"))
    {
        certWhitelistSection = "certificate_whitelist";
    }

    bool enableWhiteList{false};
    // CAL means certificate accepted list, CAL optional in config.ini
    if (_pt.get_child_optional(certWhitelistSection))
    {
        for (auto it : _pt.get_child(certWhitelistSection))
        {
            if (0 == it.first.find("cal."))
            {
                try
                {
                    std::string nodeID{boost::to_upper_copy(it.second.data())};
                    GATEWAY_CONFIG_LOG(DEBUG) << LOG_BADGE("GatewayConfig")
                                              << LOG_BADGE("get certificate accepted by nodeID")
                                              << LOG_KV("nodeID", nodeID);
                    bool isNodeIDValid =
                        (false == m_smSSL ? isNodeIDOk<h2048>(nodeID) : isNodeIDOk<h512>(nodeID));
                    if (true == isNodeIDValid)
                    {
                        m_enableWhitelist = true;
                        m_certWhitelist.emplace(std::move(nodeID));
                    }
                    else
                    {
                        GATEWAY_CONFIG_LOG(ERROR)
                            << LOG_BADGE("GatewayConfig")
                            << LOG_DESC("get certificate accepted by nodeID failed, illegal nodeID")
                            << LOG_KV("nodeID", nodeID);
                    }
                }
                catch (const std::exception& e)
                {
                    GATEWAY_CONFIG_LOG(ERROR)
                        << LOG_BADGE("GatewayConfig") << LOG_DESC("get certificate accepted failed")
                        << LOG_KV("EINFO", boost::diagnostic_information(e));
                }
            }
        }
    }
}

void GatewayConfig::checkFileExist(const std::string& _path)
{
    auto fileContent = readContentsToString(boost::filesystem::path(_path));
    if (!fileContent || fileContent->empty())
    {
        BOOST_THROW_EXCEPTION(
            InvalidParameter() << errinfo_comment("checkFileExist: unable to load file content "
                                                  " maybe file not exist, path: " +
                                                  _path));
    }
}