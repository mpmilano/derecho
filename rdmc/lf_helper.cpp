#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <list>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>
#include <GetPot>
#include <arpa/inet.h>
#include <byteswap.h>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_domain.h>

#include "derecho/connection_manager.h"
#include "derecho/derecho_ports.h"
#include "lf_helper.h"
#include "tcp/tcp.h"
#include "util.h"

#ifdef _DEBUG
#include <spdlog/spdlog.h>
#endif

/** From sst/verbs.cpp */
#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither
__LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

using namespace std;

namespace rdma {

/** Debugging tools from Weijia's sst code */  
#ifdef _DEBUG
    inline auto dbgConsole() {
        static auto console = spdlog::stdout_color_mt("console");
        return console;
    }
    #define dbg_trace(...) dbgConsole()->trace(__VA_ARGS__)
    #define dbg_debug(...) dbgConsole()->debug(__VA_ARGS__)
    #define dbg_info(...) dbgConsole()->info(__VA_ARGS__)
    #define dbg_warn(...) dbgConsole()->warn(__VA_ARGS__)
    #define dbg_error(...) dbgConsole()->error(__VA_ARGS__)
    #define dbg_crit(...) dbgConsole()->critical(__VA_ARGS__)
#else
    #define dbg_trace(...)
    #define dbg_debug(...)
    #define dbg_info(...)
    #define dbg_warn(...)
    #define dbg_error(...)
    #define dbg_crit(...)
#endif//_DEBUG
#define CRASH_WITH_MESSAGE(...) \
do { \
    fprintf(stderr,__VA_ARGS__); \
    fflush(stderr); \
    exit(-1); \
} while (0);

/** Testing tools from Weijia's sst code */
enum NextOnFailure{
    REPORT_ON_FAILURE = 0,
    CRASH_ON_FAILURE = 1
};
#define FAIL_IF_NONZERO(x,desc,next) \
    do { \
        int64_t _int64_r_ = (int64_t)(x); \
        if (_int64_r_ != 0) { \
            dbg_error("{}:{},ret={},{}",__FILE__,__LINE__,_int64_r_,desc); \
            fprintf(stderr,"%s:%d,ret=%ld,%s\n",__FILE__,__LINE__,_int64_r_,desc); \
            if (next == CRASH_ON_FAILURE) { \
                fflush(stderr); \
                exit(-1); \
            } \
        } \
    } while (0)
#define FAIL_IF_ZERO(x,desc,next) \
    do { \
        int64_t _int64_r_ = (int64_t)(x); \
        if (_int64_r_ == 0) { \
            dbg_error("{}:{},{}",__FILE__,__LINE__,desc); \
            fprintf(stderr,"%s:%d,%s\n",__FILE__,__LINE__,desc); \
            if (next == CRASH_ON_FAILURE) { \
                fflush(stderr); \
                exit(-1); \
            } \
        } \
    } while (0)

/** 
 * Passive endpoint info to be exchange
 */
#define MAX_LF_ADDR_SIZE    ((128)-sizeof(uint32_t)-2*sizeof(uint64_t))
struct cm_con_data_t {
    uint32_t  pep_addr_len;               /** local endpoint address length */
    char      pep_addr[MAX_LF_ADDR_SIZE]; /** local endpoint address */
} __attribute__((packed));

/** 
 * Object to hold the tcp connections for every node 
 */
tcp::tcp_connections *rdmc_connections;

/** 
 * Listener to detect new incoming connections 
 */
static unique_ptr<tcp::connection_listener> connection_listener;

/**
 * Vector of completion handlers and a mutex for accessing it
 */
struct completion_handler_set {
    completion_handler send;
    completion_handler recv;
    completion_handler write;
    string name;
};
static vector<completion_handler_set> completion_handlers;
static std::mutex completion_handlers_mutex;

/** 
 * Global states 
 */
struct lf_ctxt {
    struct fi_info     * hints;           /** hints */
    struct fi_info     * fi;              /** fabric information */
    struct fid_fabric  * fabric;          /** fabric handle */
    struct fid_domain  * domain;          /** domain handle */
    struct fid_pep     * pep;             /** passive endpoint for receiving connection */
    struct fid_eq      * peq;             /** event queue for connection management */
    struct fid_eq      * eq;              /** event queue for transmitting events */
    struct fid_cq      * cq;              /** completion queue for all rma operations */
    size_t             pep_addr_len;      /** length of local pep address */
    char               pep_addr[MAX_LF_ADDR_SIZE]; /** local pep address */
    struct fi_eq_attr  eq_attr;           /** event queue attributes */
    struct fi_cq_attr  cq_attr;           /** completion queue attributes */
};
/** The global context for libfabric */
struct lf_ctxt g_ctxt;

#define LF_USE_VADDR ((g_ctxt.fi->domain_attr->mr_mode) & FI_MR_VIRT_ADDR)
#define LF_CONFIG_FILE "rdma.cfg"

namespace impl {

/** 
 * Populate some of the global context with default valus 
 */
static void default_context() {
    memset((void*)&g_ctxt, 0, sizeof(struct lf_ctxt));
    /** Create a new empty fi_info structure */
    g_ctxt.hints = fi_allocinfo();
    /** Set the interface capabilities, see fi_getinfo(3) for details */
    g_ctxt.hints->caps = FI_MSG|FI_RMA|FI_READ|FI_WRITE|FI_REMOTE_READ|FI_REMOTE_WRITE;
    /** Use connection-based endpoints */
    g_ctxt.hints->ep_attr->type = FI_EP_MSG;
    /** Enable all modes */
    g_ctxt.hints->mode = ~0;
    /** Set the completion format to be user-specifed */ 
    if (g_ctxt.cq_attr.format == FI_CQ_FORMAT_UNSPEC) {
        g_ctxt.cq_attr.format = FI_CQ_FORMAT_CONTEXT;
    }
    /** Says the user will only wait on the CQ using libfabric calls */
    g_ctxt.cq_attr.wait_obj = FI_WAIT_UNSPEC;
    /** Set the size of the local pep address */
    g_ctxt.pep_addr_len = MAX_LF_ADDR_SIZE;
}

/** 
 * Load the global context from a configuration file
 */
static void load_configuration() {
    #define DEFAULT_PROVIDER "sockets"  /** Can be one of verbs|psm|sockets|usnic */
    #define DEFAULT_DOMAIN   "eth0"     /** Default domain depends on system */
    #define DEFAULT_TX_DEPTH  4096      /** Tx queue depth */
    #define DEFAULT_RX_DEPTH  4096      /** Rx queue depth */
    GetPot cfg(LF_CONFIG_FILE);         /** Load the configuration file */
    
    FAIL_IF_ZERO(g_ctxt.hints, "FI hints not allocated",  CRASH_ON_FAILURE);
    FAIL_IF_ZERO(                       /** Load the provider from config */
        g_ctxt.hints->fabric_attr->prov_name = strdup(cfg("provider", DEFAULT_PROVIDER)),
        "Failed to load the provider from config file", CRASH_ON_FAILURE
    );
    FAIL_IF_ZERO(                       /** Load the domain from config */
        g_ctxt.hints->domain_attr->name = strdup(cfg("domain", DEFAULT_DOMAIN)),
        "Failed to load the domain from config file", CRASH_ON_FAILURE
    );
    /** Set the memory region mode mode bits, see fi_mr(3) for details */
    g_ctxt.hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_ALLOCATED | 
                                         FI_MR_VIRT_ADDR;
    /** Set the tx and rx queue sizes, see fi_endpoint(3) for details */
    g_ctxt.hints->tx_attr->size = cfg("tx_depth", DEFAULT_TX_DEPTH);
    g_ctxt.hints->rx_attr->size = cfg("rx_depth", DEFAULT_RX_DEPTH);
}
}


/**
 * Memory region constructors and member functions
 */

memory_region::memory_region(size_t s) : memory_region(new char[s], s) {
    allocated_buffer.reset(buffer);
}

memory_region::memory_region(char *buf, size_t s) : buffer(buf), size(s) {
    if (!buffer || size <= 0) throw rdma::invalid_args();

    const int mr_access = FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
 
    /** Register the memory, use it to construct a smart pointer */  
    fid_mr* raw_mr;
    FAIL_IF_NONZERO(
        fi_mr_reg(g_ctxt.domain, (void *)buffer, size, mr_access, 
                  0, 0, 0, &raw_mr, nullptr),
        "Failed to register memory", CRASH_ON_FAILURE
    );
    FAIL_IF_ZERO(raw_mr, "Pointer to memory region is null", CRASH_ON_FAILURE);

    mr = unique_ptr<fid_mr, std::function<void(fid_mr *)>>(
        raw_mr, [](fid_mr *mr) { fi_close(&mr->fid); }
    ); 
}

uint64_t memory_region::get_key() const { return mr->key; }

/** 
 * Completion queue constructor
 */
completion_queue::completion_queue() {
    g_ctxt.cq_attr.size = g_ctxt.fi->tx_attr->size;
    fid_cq* raw_cq;
    FAIL_IF_NONZERO(
        fi_cq_open(g_ctxt.domain, &(g_ctxt.cq_attr), &raw_cq, NULL),
        "failed to initialize tx completion queue", CRASH_ON_FAILURE
    );
    FAIL_IF_ZERO(raw_cq, "Pointer to completion queue is null", CRASH_ON_FAILURE);
    
    cq = unique_ptr<fid_cq, std::function<void(fid_cq *)>>(
        raw_cq, [](fid_cq *cq) { fi_close(&cq->fid); }
    ); 
}

endpoint::~endpoint() {}
endpoint::endpoint(size_t remote_index, bool is_lf_server)
    : endpoint(remote_index, is_lf_server, [](endpoint *){}) {}
endpoint::endpoint(size_t remote_index, bool is_lf_server,
                   std::function<void(endpoint *)> post_recvs) { 
    connect(remote_index, is_lf_server, post_recvs); 
}

int endpoint::init(struct fi_info *fi) {
    int ret;
    /** Open an endpoint */
    fid_ep* raw_ep;
    FAIL_IF_NONZERO(
        ret = fi_endpoint(g_ctxt.domain, fi, &raw_ep, NULL), 
        "Failed to open endpoint", REPORT_ON_FAILURE
    );
    if(ret) return ret;
   
    /** Construct the smart pointer to manage the endpoint */ 
    ep = unique_ptr<fid_ep, std::function<void(fid_ep *)>>(
        raw_ep, [](fid_ep *ep) { fi_close(&ep->fid); }
    ); 
 
    /** Bind endpoint to event queue and completion queue */
    FAIL_IF_NONZERO(
        ret = fi_ep_bind(ep.get(), &(g_ctxt.eq)->fid, 0), 
        "Failed to bind endpoint and event queue", REPORT_ON_FAILURE
    );
    if(ret) return ret;
    const int ep_flags = FI_RECV | FI_TRANSMIT | FI_SELECTIVE_COMPLETION;
    FAIL_IF_NONZERO(
        ret = fi_ep_bind(ep.get(), &(g_ctxt.cq)->fid, ep_flags), 
        "Failed to bind endpoint and tx completion queue", REPORT_ON_FAILURE
    );
    if(ret) return ret;
    FAIL_IF_NONZERO(
        ret = fi_enable(ep.get()), 
        "Failed to enable endpoint", REPORT_ON_FAILURE
    );
    return ret;
}

bool sync(uint32_t r_id) {
    int s = 0, t = 0;
    return rdmc_connections->exchange(r_id, s, t);
}

void endpoint::connect(size_t remote_index, bool is_lf_server, 
                       std::function<void(endpoint *)> post_recvs) {
    struct cm_con_data_t local_cm_data, remote_cm_data;
    memset(&local_cm_data, 0, sizeof(local_cm_data));
    memset(&remote_cm_data, 0, sizeof(remote_cm_data));
    
    /** Populate local cm struct and exchange cm info */    
    local_cm_data.pep_addr_len  = (uint32_t)htonl((uint32_t)g_ctxt.pep_addr_len);
    memcpy((void*)&local_cm_data.pep_addr, &g_ctxt.pep_addr, g_ctxt.pep_addr_len);

    FAIL_IF_ZERO(
        rdmc_connections->exchange(remote_index, local_cm_data, remote_cm_data),
        "Failed to exchange cm info", CRASH_ON_FAILURE
    );

    remote_cm_data.pep_addr_len = (uint32_t)ntohl(remote_cm_data.pep_addr_len);

    /** Connect to remote node */
    ssize_t nRead;
    struct fi_eq_cm_entry entry;
    uint32_t event;

    if (is_lf_server) {
        /** Synchronously read from the passive event queue, init the server ep */ 
        nRead = fi_eq_sread(g_ctxt.peq, &event, &entry, sizeof(entry), -1, 0);
        if(nRead != sizeof(entry)) {
            CRASH_WITH_MESSAGE("Failed to get connection from remote. nRead=%ld\n",nRead);
        }
        if (init(entry.info)){
            fi_reject(g_ctxt.pep, entry.info->handle, NULL, 0);
            fi_freeinfo(entry.info);
            CRASH_WITH_MESSAGE("Failed to initialize server endpoint.\n");
        }
        if (fi_accept(ep.get(), NULL, 0)){
            fi_reject(g_ctxt.pep, entry.info->handle, NULL, 0);
            fi_freeinfo(entry.info);
            CRASH_WITH_MESSAGE("Failed to accept connection.\n");
        }
        fi_freeinfo(entry.info);
    } else {
        struct fi_info * client_hints = fi_dupinfo(g_ctxt.hints);
        struct fi_info * client_info = NULL;

        /** TODO document this */    
        FAIL_IF_ZERO(
            client_hints->dest_addr = malloc(remote_cm_data.pep_addr_len),
            "Failed to malloc address space for server pep.", CRASH_ON_FAILURE
        );
        memcpy((void*)client_hints->dest_addr,
               (void*)remote_cm_data.pep_addr,
               (size_t)remote_cm_data.pep_addr_len);
        client_hints->dest_addrlen = remote_cm_data.pep_addr_len;
        FAIL_IF_NONZERO(
            fi_getinfo(LF_VERSION, NULL, NULL, 0, client_hints, &client_info),
            "fi_getinfo() failed.", CRASH_ON_FAILURE
        );

        /** TODO document this */
        if (init(client_info)){
            fi_freeinfo(client_hints);
            fi_freeinfo(client_info);
            CRASH_WITH_MESSAGE("failed to initialize client endpoint.\n");
        }
        FAIL_IF_NONZERO(
            fi_connect(ep.get(), remote_cm_data.pep_addr, NULL, 0),
            "fi_connect() failed", CRASH_ON_FAILURE
        );
       
        /** TODO document this */
        nRead = fi_eq_sread(g_ctxt.eq, &event, &entry, sizeof(entry), -1, 0);
        if (nRead != sizeof(entry)) {
            CRASH_WITH_MESSAGE("failed to connect remote. nRead=%ld.\n",nRead);
        }
        if (event != FI_CONNECTED || entry.fid != &(ep->fid)) {
            fi_freeinfo(client_hints);
            fi_freeinfo(client_info);
            CRASH_WITH_MESSAGE("Unexpected CM event: %d.\n", event);
        }
        fi_freeinfo(client_hints);
        fi_freeinfo(client_info);
    }

    post_recvs(this);
    int tmp = -1;
    if (!rdmc_connections->exchange(remote_index, 0, tmp) || tmp != 0)
        CRASH_WITH_MESSAGE("Failed to sync after endpoint creation");
}

bool endpoint::post_send(const memory_region& mr, size_t offset, size_t size,  
                         uint64_t wr_id, uint32_t immediate, 
                         const message_type& type) {
    struct iovec msg_iov;
    struct fi_msg msg;
    
    msg_iov.iov_base = mr.buffer + offset;
    msg_iov.iov_len  = size;

    msg.msg_iov   = &msg_iov;
    msg.desc      = (void**)&mr.mr->key;
    msg.iov_count = 1;
    msg.addr      = 0;
    msg.context   = (void*)(wr_id | ((uint64_t)*type.tag << type.shift_bits)); 
    msg.data      = immediate;
 
    FAIL_IF_NONZERO(
        fi_sendmsg(ep.get(), &msg, FI_COMPLETION),
        "fi_sendmsg() failed", REPORT_ON_FAILURE
    );
    return true; 
}

bool endpoint::post_recv(const memory_region& mr, size_t offset, size_t size, 
                         uint64_t wr_id, const message_type& type) {
    struct iovec msg_iov;
    struct fi_msg msg;

    msg_iov.iov_base = mr.buffer + offset;
    msg_iov.iov_len  = size;

    msg.msg_iov   = &msg_iov;
    msg.desc      = (void**)&mr.mr->key;
    msg.iov_count = 1;
    msg.addr      = 0;
    msg.context   = (void*)(wr_id | ((uint64_t)*type.tag << type.shift_bits)); 
    msg.data      = 0;

    FAIL_IF_NONZERO(
        fi_recvmsg(ep.get(), &msg, FI_COMPLETION),
        "fi_recvmsg() failed", REPORT_ON_FAILURE
    );
    return true; 
}

bool endpoint::post_empty_send(uint64_t wr_id, uint32_t immediate,
                               const message_type& type) {
    struct fi_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.context = (void*)(wr_id | ((uint64_t)*type.tag << type.shift_bits)); 
    msg.data    = immediate;
 
    FAIL_IF_NONZERO(
        fi_sendmsg(ep.get(), &msg, FI_COMPLETION),
        "fi_sendmsg() failed", REPORT_ON_FAILURE
    );
    return true; 
}

bool endpoint::post_empty_recv(uint64_t wr_id, const message_type& type) {
    struct fi_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.context = (void*)(wr_id | ((uint64_t)*type.tag << type.shift_bits)); 
    msg.data    = 0;
 
    FAIL_IF_NONZERO(
        fi_recvmsg(ep.get(), &msg, FI_COMPLETION),
        "fi_recvmsg() failed", REPORT_ON_FAILURE
    );
    return true; 
}

bool endpoint::post_write(const memory_region& mr, size_t offset, size_t size,
                          uint64_t wr_id, remote_memory_region remote_mr,
                          size_t remote_offset, const message_type& type,  
                          bool signaled, bool send_inline) {
   if(wr_id >> type.shift_bits || !type.tag) throw invalid_args();
    if(mr.size < offset + size || remote_mr.size < remote_offset + size) {
        cout << "mr.size = " << mr.size << " offset = " << offset
             << " length = " << size << " remote_mr.size = " << remote_mr.size
             << " remote_offset = " << remote_offset;
        return false;
    }
  
    struct iovec msg_iov;
    struct fi_rma_iov rma_iov;
    struct fi_msg_rma msg;

    msg_iov.iov_base = mr.buffer + offset;
    msg_iov.iov_len  = size;

    rma_iov.addr = ((LF_USE_VADDR) ? remote_mr.buffer : 0) + remote_offset;
    rma_iov.len  = size;
    rma_iov.key  = remote_mr.rkey;

    msg.msg_iov       = &msg_iov;
    msg.desc          = (void**)&mr.mr->key;
    msg.iov_count     = 1;
    msg.addr          = 0;
    msg.rma_iov       = &rma_iov;
    msg.rma_iov_count = 1;
    msg.context       = (void*)(wr_id | ((uint64_t)*type.tag << type.shift_bits)); 
    msg.data          = 0l;

    FAIL_IF_NONZERO(
        fi_writemsg(ep.get(), &msg, FI_COMPLETION),
        "fi_writemsg() failed", REPORT_ON_FAILURE
    );

    return true;
}

message_type::message_type(const std::string& name, completion_handler send_handler,
                           completion_handler recv_handler,
                 completion_handler write_handler) {

    std::lock_guard<std::mutex> l(completion_handlers_mutex);

    //if(completion_handlers.size() >= std::numeric_limits<tag_type>::max())
    //    throw message_types_exhausted();

    tag = completion_handlers.size();

    completion_handler_set set;
    set.send = send_handler;
    set.recv = recv_handler;
    set.write = write_handler;
    set.name = name;
    completion_handlers.push_back(set);
}

message_type message_type::ignored() {
    static message_type m(std::numeric_limits<tag_type>::max());
    return m;
}

struct task::task_impl {
  int dummy;
};

task::task(std::shared_ptr<manager_endpoint> manager_ep) {
     return;
}

task::~task() {}

void task::append_wait(const completion_queue& cq, int count, bool signaled,
                 bool last, uint64_t wr_id, const message_type& type) {
    throw unsupported_feature();
}

void task::append_enable_send(const managed_endpoint& ep, int count) {
    throw unsupported_feature();
}

void task::append_send(const managed_endpoint& ep, const memory_region& mr,
                 size_t offset, size_t length, uint32_t immediate) {
    throw unsupported_feature();
}
void task::append_recv(const managed_endpoint& ep, const memory_region& mr,
                 size_t offset, size_t length) {
    throw unsupported_feature();
}

bool task::post() {
    throw unsupported_feature();
}

namespace impl {
/**
 * Adds a node to the group via tcp
 */
bool lf_add_connection(uint32_t new_id, const std::string new_ip_addr) {
   return rdmc_connections->add_node(new_id, new_ip_addr);
}

static atomic<bool> interrupt_mode;
//static atomic<bool> polling_loop_shutdown_flag;
static void polling_loop() {
/*    pthread_setname_np(pthread_self(), "rdmc_poll");

    const int max_cq_entries = 1024;
    unique_ptr<fi_cq_entry[]> cq_entires(new fi_cq_entry[max_cq_entires]);

    while(true) {
        int num_completions = 0;
        if (polling_loop_shutdown_flag) return;
            uint64_t poll_end = get_time() + (interrupt_mode ? 0L : 50000000L);
            do {
                if(polling_loop_shutdown_flag) return;
                num_completions = fi_cq_read(g_ctxt.cq, cq_entries.get() max_cq_entries);
            } while(num_completions == 0 && get_time() < poll_end);
    }  */
}

/**
 * Initialize the global context 
 */
bool lf_initialize( 
    const std::map<uint32_t, std::string>& node_addrs, uint32_t node_rank) {
   
    /** Initialize the connection listener on the rdmc tcp port */     
    connection_listener = make_unique<tcp::connection_listener>(derecho::rdmc_tcp_port);
    
    /** Initialize the tcp connections, also connects all the nodes together */
    rdmc_connections = new tcp::tcp_connections(node_rank, node_addrs, derecho::rdmc_tcp_port);
    
    /** Set the context to defaults to start with */
    default_context();
    load_configuration();  
   
    dbg_info(fi_tostr(g_ctxt.hints, FI_TYPE_INFO)); 

    /** Initialize the fabric, domain and completion queue */ 
    FAIL_IF_NONZERO(
        fi_getinfo(LF_VERSION, NULL, NULL, 0, g_ctxt.hints, &(g_ctxt.fi)),
        "fi_getinfo() failed", CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        fi_fabric(g_ctxt.fi->fabric_attr, &(g_ctxt.fabric), NULL),
        "fi_fabric() failed", CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        fi_domain(g_ctxt.fabric, g_ctxt.fi, &(g_ctxt.domain), NULL),
        "fi_domain() failed", CRASH_ON_FAILURE
    );

    /** Initialize the event queue, initialize and configure pep  */
    FAIL_IF_NONZERO(
        fi_eq_open(g_ctxt.fabric, &g_ctxt.eq_attr, &g_ctxt.peq, NULL),
        "failed to open the event queue for passive endpoint",CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        fi_passive_ep(g_ctxt.fabric, g_ctxt.fi, &g_ctxt.pep, NULL),
        "failed to open a local passive endpoint", CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        fi_pep_bind(g_ctxt.pep, &g_ctxt.peq->fid, 0),
        "failed to bind event queue to passive endpoint", CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        fi_listen(g_ctxt.pep), 
        "failed to prepare passive endpoint for incoming connections", CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        fi_getname(&g_ctxt.pep->fid, g_ctxt.pep_addr, &g_ctxt.pep_addr_len),
        "failed to get the local PEP address", CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        (g_ctxt.pep_addr_len > MAX_LF_ADDR_SIZE),
        "local name is too big to fit in local buffer",CRASH_ON_FAILURE
    );
    FAIL_IF_NONZERO(
        fi_eq_open(g_ctxt.fabric, &g_ctxt.eq_attr, &g_ctxt.eq, NULL),
        "failed to open the event queue for rdma transmission.", CRASH_ON_FAILURE
    );

    /** Start a polling thread and run in the background */
    std::thread polling_thread(polling_loop);
    polling_thread.detach();

    return true;
}

bool lf_destroy() {
  return false;
}

std::map<uint32_t, remote_memory_region> lf_exchange_memory_regions(
         const std::vector<uint32_t>& members, uint32_t node_rank,
         const memory_region& mr) {
    /** Maps a node's ID to a memory region on that node */
    map<uint32_t, remote_memory_region> remote_mrs;
    for (uint32_t m : members) {
        if (m == node_rank) {
            continue;
        }

        uint64_t buffer;
        size_t size;
        uint64_t rkey;
        
        if(!rdmc_connections->exchange(m, (uint64_t)mr.buffer, buffer) || 
           !rdmc_connections->exchange(m, mr.size, size) ||
           !rdmc_connections->exchange(m, mr.get_key(), rkey)) {
            fprintf(stderr, "WARNING: lost connection to node %u\n", m);
            throw rdma::connection_broken();
        }
        remote_mrs.emplace(m, remote_memory_region(buffer, size, rkey));
    }
    return remote_mrs;
}

bool set_interrupt_mode(bool enabled) {
    interrupt_mode = enabled;
    return true;
}

}/* namespace impl */
}/* namespace rdma */
