#include <map>
using namespace std;

#ifdef __cplusplus
extern "C" {
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>

#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/if_ether.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
//#include <cJSON.h>
#include <unistd.h>
#include <netinet/in.h>

#include <openair2/COMMON/platform_types.h>
#include <openair3/UTILS/conversions.h>
#include "common/utils/LOG/log.h"
#include <common/utils/ocp_itti/intertask_interface.h>
#include <openair2/COMMON/gtpv1_u_messages_types.h>
#include <openair3/ocp-gtpu/gtp_itf.h>
#include <openair2/LAYER2/PDCP_v10.1.0/pdcp.h>
#include <openair2/LAYER2/nr_pdcp/nr_pdcp_oai_api.h>
#include <openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h>
#include "openair2/SDAP/nr_sdap/nr_sdap.h"
#include "sim.h"
#include <pthread.h>
#include <stdio.h>
#include <math.h>
// #include <iostream>

#pragma pack(1)

#define PDCP_HASH_TABLE_SIZE  1 << 24
uint8_t pdcp[PDCP_HASH_TABLE_SIZE][3];
#define UE_HASH_TABLE_SIZE  1 << 24
ue_id_t UE_CPE[UE_HASH_TABLE_SIZE];

void initialize_pdcp_array() {
  for (int i = 0; i < PDCP_HASH_TABLE_SIZE; i++) {
    if (i == (PDCP_HASH_TABLE_SIZE - 1)){
      printf("pdcp finish\n");
    }
    pdcp[i][0] = 0x80;
    pdcp[i][1] = 0x00;
    pdcp[i][2] = 0x00;
  }
}

typedef struct Gtpv1uMsgHeader {
  uint8_t PN:1;
  uint8_t S:1;
  uint8_t E:1;
  uint8_t spare:1;
  uint8_t PT:1;
  uint8_t version:3;
  uint8_t msgType;
  uint16_t msgLength;
  teid_t teid;
} __attribute__((packed)) Gtpv1uMsgHeaderT;

//TS 38.425, Figure 5.5.2.2-1
typedef struct DlDataDeliveryStatus_flags {
  uint8_t LPR:1;                    //Lost packet report
  uint8_t FFI:1;                    //Final Frame Ind
  uint8_t deliveredPdcpSn:1;        //Highest Delivered NR PDCP SN Ind
  uint8_t transmittedPdcpSn:1;      //Highest Transmitted NR PDCP SN Ind
  uint8_t pduType:4;                //PDU type
  uint8_t CR:1;                     //Cause Report
  uint8_t deliveredReTxPdcpSn:1;    //Delivered retransmitted NR PDCP SN Ind
  uint8_t reTxPdcpSn:1;             //Retransmitted NR PDCP SN Ind
  uint8_t DRI:1;                    //Data Rate Indication
  uint8_t deliveredPdcpSnRange:1;   //Delivered NR PDCP SN Range Ind
  uint8_t spare:3;
  uint32_t drbBufferSize;            //Desired buffer size for the data radio bearer
} __attribute__((packed)) DlDataDeliveryStatus_flagsT;

typedef struct Gtpv1uMsgHeaderOptFields {
  uint8_t seqNum1Oct;
  uint8_t seqNum2Oct;
  uint8_t NPDUNum;
  uint8_t NextExtHeaderType;    
} __attribute__((packed)) Gtpv1uMsgHeaderOptFieldsT;

#define DL_PDU_SESSION_INFORMATION 0
#define UL_PDU_SESSION_INFORMATION 1

  typedef struct PDUSessionContainer {
  uint8_t spare:4;
  uint8_t PDU_type:4;
  uint8_t QFI:6;
  uint8_t Reflective_QoS_activation:1;
  uint8_t Paging_Policy_Indicator:1;
} __attribute__((packed)) PDUSessionContainerT;

typedef struct Gtpv1uExtHeader {
  uint8_t ExtHeaderLen;
  PDUSessionContainerT pdusession_cntr;
  uint8_t NextExtHeaderType;
}__attribute__((packed)) Gtpv1uExtHeaderT;

#pragma pack()

// TS 29.281, fig 5.2.1-3
#define PDU_SESSION_CONTAINER       (0x85)
#define NR_RAN_CONTAINER            (0x84)

// TS 29.281, 5.2.1
#define EXT_HDR_LNTH_OCTET_UNITS    (4)
#define NO_MORE_EXT_HDRS            (0)

// TS 29.060, table 7.1 defines the possible message types
// here are all the possible messages (3GPP R16)
#define GTP_ECHO_REQ                                         (1)
#define GTP_ECHO_RSP                                         (2)
#define GTP_ERROR_INDICATION                                 (26)
#define GTP_SUPPORTED_EXTENSION_HEADER_INDICATION            (31)
#define GTP_END_MARKER                                       (254)
#define GTP_GPDU                                             (255)

typedef struct gtpv1u_bearer_s {
  /* TEID used in dl and ul */
  teid_t          teid_incoming;                ///< eNB TEID
  teid_t          teid_outgoing;                ///< Remote TEID
  in_addr_t       outgoing_ip_addr;
  struct in6_addr outgoing_ip6_addr;
  tcp_udp_port_t  outgoing_port;
  uint16_t        seqNum;
  uint8_t         npduNum;
  int outgoing_qfi;
} gtpv1u_bearer_t;

typedef struct {
  map<ue_id_t, gtpv1u_bearer_t> bearers;
  teid_t outgoing_teid;
} teidData_t;

typedef struct {
  ue_id_t ue_id;
  ebi_t incoming_rb_id;
  gtpCallback callBack;
  teid_t outgoing_teid;
  gtpCallbackSDAP callBackSDAP;
  int pdusession_id;
} ueidData_t;

class gtpEndPoint {
 public:
  openAddr_t addr;
  uint8_t foundAddr[20];
  int foundAddrLen;
  int ipVersion;
  map<uint64_t, teidData_t> ue2te_mapping;
  // we use the same port number for source and destination address
  // this allow using non standard gtp port number (different from 2152)
  // and so, for example tu run 4G and 5G cores on one system
  tcp_udp_port_t get_dstport() {
    return (tcp_udp_port_t)atol(addr.destinationService);
  }
};

class gtpEndPoints {
 public:
  pthread_mutex_t gtp_lock=PTHREAD_MUTEX_INITIALIZER;
  // the instance id will be the Linux socket handler, as this is uniq
  map<uint64_t, gtpEndPoint> instances;
  map<uint64_t, ueidData_t> te2ue_mapping;
  gtpEndPoints() {
    unsigned int seed;
    fill_random(&seed, sizeof(seed));
    srandom(seed);
  }

  ~gtpEndPoints() {
    // automatically close all sockets on quit
    for (const auto &p : instances)
      close(p.first);
  }
};

gtpEndPoints globGtp;

// note TEid 0 is reserved for specific usage: echo req/resp, error and supported extensions
static  teid_t gtpv1uNewTeid(void) {
#ifdef GTPV1U_LINEAR_TEID_ALLOCATION
  g_gtpv1u_teid = g_gtpv1u_teid + 1;
  return g_gtpv1u_teid;
#else
  return random() + random() % (RAND_MAX - 1) + 1;
#endif
}

instance_t legacyInstanceMapping=0;
#define compatInst(a) ((a)==0 || (a)==INSTANCE_DEFAULT ? legacyInstanceMapping:a)

#define getInstRetVoid(insT)                                    \
  auto instChk=globGtp.instances.find(compatInst(insT));    \
  if (instChk == globGtp.instances.end()) {                        \
    LOG_E(GTPU,"try to get a gtp-u not existing output\n");     \
    pthread_mutex_unlock(&globGtp.gtp_lock);                    \
    return;                                                     \
  }                                                             \
  gtpEndPoint * inst=&instChk->second;
  
#define getInstRetInt(insT)                                    \
  auto instChk=globGtp.instances.find(compatInst(insT));    \
  if (instChk == globGtp.instances.end()) {                        \
    LOG_E(GTPU,"try to get a gtp-u not existing output\n");     \
    pthread_mutex_unlock(&globGtp.gtp_lock);                    \
    return GTPNOK;                                                     \
  }                                                             \
  gtpEndPoint * inst=&instChk->second;


#define getUeRetVoid(insT, Ue)                                            \
    auto ptrUe=insT->ue2te_mapping.find(Ue);                        \
                                                                        \
  if (  ptrUe==insT->ue2te_mapping.end() ) {                          \
    LOG_E(GTPU, "[%ld] gtpv1uSend failed: while getting ue id %ld in hashtable ue_mapping\n", instance, ue_id); \
    pthread_mutex_unlock(&globGtp.gtp_lock);                            \
    return;                                                             \
  }
  
#define getUeRetInt(insT, Ue)                                            \
    auto ptrUe=insT->ue2te_mapping.find(Ue);                        \
                                                                        \
  if (  ptrUe==insT->ue2te_mapping.end() ) {                          \
    LOG_E(GTPU, "[%ld] gtpv1uSend failed: while getting ue id %ld in hashtable ue_mapping\n", instance, ue_id); \
    pthread_mutex_unlock(&globGtp.gtp_lock);                            \
    return GTPNOK;                                                             \
  }

#define ARP_HASH_TABLE_SIZE  1 << 24
#define ARP_HASK_TABLE_INDEX_MASK (ARP_HASH_TABLE_SIZE - 1)

struct device_arp_table_entry {
  uint8_t device_mac_addr[6];
  uint32_t device_ip;
};

unsigned int hash_arp(uint32_t device_ip);

int device_arp_table_insert(uint8_t device_mac_addr[6], uint32_t device_ip);
struct device_arp_table_entry* device_arp_table_get_entry_by_ul_ip(uint32_t hash);

static struct device_arp_table_entry *arp_hash_table[ARP_HASH_TABLE_SIZE];

int device_arp_table_insert(uint8_t device_mac_addr[6], uint32_t device_ip)
{
  uint32_t table_idx = ARP_HASK_TABLE_INDEX_MASK;
  struct device_arp_table_entry *entry;
  table_idx = hash_arp(device_ip);

  entry = device_arp_table_get_entry_by_ul_ip(table_idx);

  if (entry) {
    memcpy(entry->device_mac_addr, device_mac_addr, 6);
    entry->device_ip = device_ip;

    arp_hash_table[table_idx] = entry;
    return 0;
  }

  entry = (struct device_arp_table_entry *)malloc(sizeof(struct device_arp_table_entry));
  if (entry == NULL){
    printf("error when allocate space\n");
  }

  memcpy(entry->device_mac_addr, device_mac_addr, 6);
  entry->device_ip = device_ip;

  arp_hash_table[table_idx] = entry;

  return 0;
}

struct device_arp_table_entry* device_arp_table_get_entry_by_ul_ip(uint32_t hash)
{
  struct device_arp_table_entry *cp;

  cp = arp_hash_table[hash];
  if (cp) {
    return cp;
  }

  return 0;
}

unsigned int hash_arp(uint32_t device_ip){
  unsigned int hash = 0;
    // for (int i = 0; i < 6; i++){
    //  hash += device_mac_addr[i];
    //  hash *= 5;
    // }
    //printf("IP:%u\n", device_ip);
  hash = device_ip % 1000000;
    //printf("first:%u\n", hash);
  return hash;
}

#define Device_HASH_TABLE_SIZE  1 << 24
#define Device_HASK_TABLE_INDEX_MASK (Device_HASH_TABLE_SIZE - 1)

struct device_table_entry {
  uint8_t device_mac_addr[6];
  uint32_t cpe_ip;
  uint32_t device_ip;
};

unsigned int hash_mac(uint8_t device_mac_addr[6]);

int device_table_insert(uint8_t device_mac_addr[6], uint32_t cpe_ip, uint32_t device_ip);
struct device_table_entry* device_table_get_entry_by_ul_mac(uint32_t hash);
static struct device_table_entry *device_hash_table[Device_HASH_TABLE_SIZE];

int device_table_insert(uint8_t device_mac_addr[6], uint32_t cpe_ip, uint32_t device_ip)
{
  uint32_t table_idx = Device_HASK_TABLE_INDEX_MASK;
  struct device_table_entry *entry;
  table_idx = hash_mac(device_mac_addr);
  entry = device_table_get_entry_by_ul_mac(table_idx);

  if (entry) {
    memcpy(entry->device_mac_addr, device_mac_addr, 6);
    entry->cpe_ip = cpe_ip;
    entry->device_ip = device_ip;

    device_hash_table[table_idx] = entry;
    return 0;
  }
  entry = (struct device_table_entry *)malloc(sizeof(struct device_table_entry));
  if (entry == NULL){
    printf("error when allocate space\n");
  }
  memcpy(entry->device_mac_addr, device_mac_addr, 6);
  entry->cpe_ip = cpe_ip;
  entry->device_ip = device_ip;

  device_hash_table[table_idx] = entry;
  if (entry == NULL)
    printf("error when insert device info\n");

  return 0;
}

struct device_table_entry* device_table_get_entry_by_ul_mac(uint32_t hash)
{
  struct device_table_entry *cp;

  cp = device_hash_table[hash];
  if (cp) {
    return cp;
  }
  return 0;
}

unsigned int hash_mac(uint8_t device_mac_addr[6]){
  unsigned int hash = 0;
  for (int i = 0; i < 6; i++){
    hash += device_mac_addr[i];
    hash *= 5;
  }
  return hash;
}

void dump_data(uint8_t *data, int len)
{
  int i;
  int j;
  int k;
  for(i=0;i<len;i+=16) {
    printf("%8.8x ", i);
    k = len - i;
    if (k > 16)
      k = 16;
    k += i;
    for(j=i; j<(i+16); j++) {
      if (j < k)
        printf("%2.2x ", data[j] );
      else
        printf("   ");
    }
    printf("  ");
    for(j=i; j<k; j++) {
      printf("%c", isprint(data[j]) ? data[j] : '.' );
    }
    printf("\n");
  }
}

#define BUFFER_SIZE 65536
int rawsockfd;
struct sockaddr_ll sll;
static int createrawsocket(){
  rawsockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (rawsockfd == -1) {
    perror("socket");
    return -1;
  }

  // Bind to a specific network interface
  memset(&sll, 0, sizeof(sll));
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex = if_nametoindex("rename2");
  if (bind(rawsockfd, (struct sockaddr *)&sll, sizeof(sll)) == -1) {
    perror("bind");
    close(rawsockfd);
    return -1;
  }
  return 0;
}

#define HDR_MAX 256 // 256 is supposed to be larger than any gtp header
static int gtpv1uCreateAndSendMsg(int h,
                                  uint32_t peerIp,
                                  uint16_t peerPort,
                                  int msgType,
                                  teid_t teid,
                                  uint8_t *Msg,
                                  int msgLen,
                                  bool seqNumFlag,
                                  bool npduNumFlag,
                                  int seqNum,
                                  int npduNum,
                                  int extHdrType,
                                  uint8_t *extensionHeader_buffer,
                                  uint8_t extensionHeader_length) {
  LOG_D(GTPU, "Peer IP:%u peer port:%u outgoing teid:%u \n", peerIp, peerPort, teid);

  uint8_t buffer[msgLen+HDR_MAX]; 
  uint8_t *curPtr=buffer;
  Gtpv1uMsgHeaderT      *msgHdr = (Gtpv1uMsgHeaderT *)buffer ;
  // N should be 0 for us (it was used only in 2G and 3G)
  msgHdr->PN=npduNumFlag;
  msgHdr->S=seqNumFlag;
  msgHdr->E = extHdrType != NO_MORE_EXT_HDRS;
  msgHdr->spare=0;
  //PT=0 is for GTP' TS 32.295 (charging)
  msgHdr->PT=1;
  msgHdr->version=1;
  msgHdr->msgType=msgType;
  msgHdr->teid=htonl(teid);

  curPtr+=sizeof(Gtpv1uMsgHeaderT);

  if (seqNumFlag || (extHdrType != NO_MORE_EXT_HDRS) || npduNumFlag) {
    *(uint16_t *)curPtr = seqNumFlag ? seqNum : 0x0000;
    curPtr+=sizeof(uint16_t);
    *(uint8_t *)curPtr = npduNumFlag ? npduNum : 0x00;
    curPtr++;
    *(uint8_t *)curPtr = extHdrType;
    curPtr++;
  }

  // Bug: if there is more than one extension, infinite loop on extensionHeader_buffer
  while (extHdrType != NO_MORE_EXT_HDRS) {
    if (extensionHeader_length > 0) {
      memcpy(curPtr, extensionHeader_buffer, extensionHeader_length);
      curPtr += extensionHeader_length;
      LOG_D(GTPU, "Extension Header for DDD added. The length is: %d, extension header type is: %x \n", extensionHeader_length, *((uint8_t *)(buffer + 11)));
      extHdrType = extensionHeader_buffer[extensionHeader_length - 1];
      LOG_D(GTPU, "Next extension header type is: %x \n", *((uint8_t *)(buffer + 11)));
    } else {
      LOG_W(GTPU, "Extension header type not supported, returning... \n");
    }
  }

  if (Msg!= NULL){
    memcpy(curPtr, Msg, msgLen);
    curPtr+=msgLen;
  }

  msgHdr->msgLength = htons(curPtr-(buffer+sizeof(Gtpv1uMsgHeaderT)));
  AssertFatal(curPtr-(buffer+msgLen) < HDR_MAX, "fixed max size of all headers too short");
  // Fix me: add IPv6 support, using flag ipVersion
  struct sockaddr_in to= {0};
  to.sin_family      = AF_INET;
  to.sin_port        = htons(peerPort);
  to.sin_addr.s_addr = peerIp ;
  LOG_D(GTPU,"sending packet size: %ld to %s\n",curPtr-buffer, inet_ntoa(to.sin_addr) );
  int ret;

  if ((ret=sendto(h, (void *)buffer, curPtr-buffer, 0,(struct sockaddr *)&to, sizeof(to) )) != curPtr-buffer ) {
    LOG_E(GTPU, "[SD %d] Failed to send data to " IPV4_ADDR " on port %d, buffer size %lu, ret: %d, errno: %d\n",
          h, IPV4_ADDR_FORMAT(peerIp), peerPort, curPtr-buffer, ret, errno);
    return GTPNOK;
  }

  return  !GTPNOK;
}

#define SRC_MAC "\xac\x4d\x54\x47\x73\x27" // Your source MAC address
#define DST_MAC "\x78\x2b\xcb\x4a\x1a\xa6" // Your destination MAC address
int temp_instance;

struct gre_header {
  uint16_t res2:4;
  uint16_t s:1;
  uint16_t k:1;
  uint16_t res1:1;
  uint16_t c:1;
  uint16_t ver:3;
  uint16_t res3:5;
  uint16_t proto;
};

unsigned short checksum5(unsigned short *buf, int nwords) {
    int sum;
    for(sum=0; nwords>0; nwords--)
        sum += *buf++;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return ~sum;
}

//ue_id_t temp_ue_id;
//int temp_bearer_id;
static void gtpv1uSend(instance_t instance, gtpv1u_tunnel_data_req_t *req, bool seqNumFlag, bool npduNumFlag) {
  uint8_t *buffer=req->buffer+req->offset;
  size_t length=req->length;

  dump_data(buffer, length);

  ue_id_t ue_id=req->ue_id;
  int bearer_id=req->bearer_id;
  //printf("ue_id: %u\n", ue_id);
  //printf("bearer_id: %d\n", bearer_id);
  //temp_ue_id = ue_id;
  //printf("temp_ue_id: %u\n", temp_ue_id);
  //temp_bearer_id = bearer_id;

  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetVoid(compatInst(instance));
  getUeRetVoid(inst, ue_id);

  auto ptr2=ptrUe->second.bearers.find(bearer_id);

  if ( ptr2 == ptrUe->second.bearers.end() ) {
    LOG_E(GTPU,"[%ld] GTP-U instance: sending a packet to a non existant UE:RAB: %lx/%x\n", instance, ue_id, bearer_id);
    pthread_mutex_unlock(&globGtp.gtp_lock);
    return;
  }

  LOG_D(GTPU,"[%ld] sending a packet to UE:RAB:teid %lx/%x/%x, len %lu, oldseq %d, oldnum %d\n",
        instance, ue_id, bearer_id,ptr2->second.teid_outgoing,length, ptr2->second.seqNum,ptr2->second.npduNum );

  if(seqNumFlag)
    ptr2->second.seqNum++;

  if(npduNumFlag)
    ptr2->second.npduNum++;

  // copy to release the mutex
  gtpv1u_bearer_t tmp=ptr2->second;
  pthread_mutex_unlock(&globGtp.gtp_lock);
  if (buffer[0] == 0x45){ //divide IPv6
    //printf("haha\n");
    struct ip *ip_hdr = (struct ip*)(buffer);
    uint32_t ip_addr = ip_hdr->ip_src.s_addr;
    uint32_t temp = ip_addr % 1000000;
    UE_CPE[temp] = ue_id;
    if (buffer[22] == 0x65 && buffer[23] == 0x58){ //Confirm GRE packet
      if ((buffer[59] == 0x44 && buffer[61] == 0x43) || (buffer[36] == 0x08 && buffer[37] == 0x06 && buffer[65] == 0x01)){ //DHCP & ARP to IND Box
        unsigned char packet[1024];
        memset(packet, 0, 1024);

        // Ethernet header
        struct ether_header *eth_header = (struct ether_header *)packet;
        memcpy(eth_header->ether_dhost, DST_MAC, ETH_ALEN);
        memcpy(eth_header->ether_shost, SRC_MAC, ETH_ALEN);
        eth_header->ether_type = htons(ETH_P_IP);

        memcpy(packet + sizeof(struct ether_header), buffer, length);

        int packet_len = sizeof(struct ether_header) + length;

        if (sendto(rawsockfd, packet, packet_len, 0, (struct sockaddr*)&sll, sizeof(sll)) == -1) {
          perror("sendto");
          close(rawsockfd);
          exit(EXIT_FAILURE);
        }
      }
      else if (buffer[54] == 0xc0 && buffer[55] == 0xa8 && buffer[56] == 0x01 && buffer[57] != 0x01){
        struct device_table_entry *entry;
        unsigned char *packet = buffer;
        struct iphdr *ip_hdr_gre = (struct iphdr *)(packet);
        ip_hdr_gre->check = 0;
        //struct iphdr *ip_hdr = (struct iphdr *)(packet + sizeof(struct iphdr) + sizeof(struct gre_header) + sizeof(struct ether_header));
        struct ether_header *eth_hdr = (struct ether_header *)(packet + sizeof(struct iphdr) + sizeof(struct gre_header));
        uint32_t hash = hash_mac(eth_hdr->ether_dhost);
        //packet += sizeof(struct ether_header);
        //len -= sizeof(struct ether_header);

        //uint32_t temp2 = ip_dst_addr % 1000000;
        entry = device_table_get_entry_by_ul_mac(hash);
        if (entry == 0)
          printf("device info not found\n");
        ip_hdr_gre->saddr = inet_addr("10.60.0.99");;
        ip_hdr_gre->daddr = entry->cpe_ip;
        ip_hdr_gre->check = checksum5((unsigned short *)ip_hdr_gre, sizeof(struct iphdr) / 2);
        uint32_t temp3 = entry->cpe_ip % 1000000;
        length += 3;
        //printf("temp: %u\n", temp);
        size_t prepend_length = sizeof(pdcp[temp3]);
        memmove(packet + prepend_length, packet, length);
        memcpy(packet, pdcp[temp3], prepend_length);
        pdcp[temp3][2]++;
        if (pdcp[temp3][2] == 0) {
          pdcp[temp3][1]++;
        }
        //dump_data(packet, len);              
        auto instChk=globGtp.instances.find(compatInst(compatInst(temp_instance)));
        if (instChk == globGtp.instances.end()) {                        
          LOG_E(GTPU,"try to get a gtp-u not existing output\n");     
          pthread_mutex_unlock(&globGtp.gtp_lock);
          //printf("fuck\n");                                                            
        }
        else{
          //printf("get instance\n");
          if (&instChk->second){
            gtpEndPoint * inst=&instChk->second;
            auto ptrUe=inst->ue2te_mapping.find(UE_CPE[temp3]);                                                                    
            if ( ptrUe==inst->ue2te_mapping.end() ) {                          
              pthread_mutex_unlock(&globGtp.gtp_lock);
              //printf("fuck2\n");                                                                                   
            }
            //printf("ue id\n");
            //getUeRetVoid(inst, 15729); //ue_id
            auto ptr2=ptrUe->second.bearers.find(1); //bearer_id
            tmp=ptr2->second;
            //printf("get bearer id\n");
            gtpv1uCreateAndSendMsg(compatInst(temp_instance),
                                 tmp.outgoing_ip_addr,
                                 tmp.outgoing_port,
                                 GTP_GPDU,
                                 tmp.teid_outgoing,
                                 packet,
                                 length, 
                                 seqNumFlag,
                                 npduNumFlag, 
                                 0, 
                                 0, 
                                 NO_MORE_EXT_HDRS, 
                                 NULL, 
                                 0);
          }
            //printf("finish\n");
        }
      }
    }
    else{
      if (tmp.outgoing_qfi != -1) {
        Gtpv1uExtHeaderT ext = { 0 };
        ext.ExtHeaderLen = 1; // in quad bytes  EXT_HDR_LNTH_OCTET_UNITS
        ext.pdusession_cntr.spare = 0;
        ext.pdusession_cntr.PDU_type = UL_PDU_SESSION_INFORMATION;
        ext.pdusession_cntr.QFI = tmp.outgoing_qfi;
        ext.pdusession_cntr.Reflective_QoS_activation = false;
        ext.pdusession_cntr.Paging_Policy_Indicator = false;
        ext.NextExtHeaderType = NO_MORE_EXT_HDRS;

        gtpv1uCreateAndSendMsg(compatInst(instance),
                               tmp.outgoing_ip_addr,
                               tmp.outgoing_port,
                               GTP_GPDU,
                               tmp.teid_outgoing,
                               buffer,
                               length,
                               seqNumFlag,
                               npduNumFlag,
                               tmp.seqNum,
                               tmp.npduNum,
                               PDU_SESSION_CONTAINER,
                               (uint8_t *)&ext,
                               sizeof(ext));
      } 
      else {
        gtpv1uCreateAndSendMsg(compatInst(instance), 
                               tmp.outgoing_ip_addr, 
                               tmp.outgoing_port, 
                               GTP_GPDU, 
                               tmp.teid_outgoing, 
                               buffer, 
                               length, 
                               seqNumFlag, 
                               npduNumFlag, 
                               tmp.seqNum, 
                               tmp.npduNum, 
                               NO_MORE_EXT_HDRS, 
                               NULL, 
                               0);
      }
    }
  }
}

static void fillDlDeliveryStatusReport(extensionHeader_t *extensionHeader, uint32_t RLC_buffer_availability, uint32_t NR_PDCP_PDU_SN){

  extensionHeader->buffer[0] = (1+sizeof(DlDataDeliveryStatus_flagsT)+(NR_PDCP_PDU_SN>0?3:0)+(NR_PDCP_PDU_SN>0?1:0)+1)/4;
  DlDataDeliveryStatus_flagsT DlDataDeliveryStatus;
  DlDataDeliveryStatus.deliveredPdcpSn = 0;
  DlDataDeliveryStatus.transmittedPdcpSn= NR_PDCP_PDU_SN>0?1:0;
  DlDataDeliveryStatus.pduType = 1;
  DlDataDeliveryStatus.drbBufferSize = htonl(RLC_buffer_availability);
  memcpy(extensionHeader->buffer+1, &DlDataDeliveryStatus, sizeof(DlDataDeliveryStatus_flagsT));
  uint8_t offset = sizeof(DlDataDeliveryStatus_flagsT)+1;

  if(NR_PDCP_PDU_SN>0){
    extensionHeader->buffer[offset] =   (NR_PDCP_PDU_SN >> 16) & 0xff;
    extensionHeader->buffer[offset+1] = (NR_PDCP_PDU_SN >> 8) & 0xff;
    extensionHeader->buffer[offset+2] = NR_PDCP_PDU_SN & 0xff;
    LOG_D(GTPU, "Octets reporting NR_PDCP_PDU_SN, extensionHeader->buffer[offset]: %u, extensionHeader->buffer[offset+1]:%u, extensionHeader->buffer[offset+2]:%u \n", extensionHeader->buffer[offset], extensionHeader->buffer[offset+1],extensionHeader->buffer[offset+2]);
    extensionHeader->buffer[offset+3] = 0x00; //Padding octet
    offset = offset+3;
  }
  extensionHeader->buffer[offset] = 0x00; //No more extension headers
  /*Total size of DDD_status PDU = size of mandatory part +
   * 3 octets for highest transmitted/delivered PDCP SN +
   * 1 octet for padding + 1 octet for next extension header type,
   * according to TS 38.425: Fig. 5.5.2.2-1 and section 5.5.3.24*/
  extensionHeader->length  = 1+sizeof(DlDataDeliveryStatus_flagsT)+
                              (NR_PDCP_PDU_SN>0?3:0)+
                              (NR_PDCP_PDU_SN>0?1:0)+1;
}

static void gtpv1uSendDlDeliveryStatus(instance_t instance, gtpv1u_DU_buffer_report_req_t *req){
  ue_id_t ue_id=req->ue_id;
  int  bearer_id=req->pdusession_id;
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetVoid(compatInst(instance));
  getUeRetVoid(inst, ue_id);

  auto ptr2=ptrUe->second.bearers.find(bearer_id);

  if ( ptr2 == ptrUe->second.bearers.end() ) {
    LOG_D(GTPU,"GTP-U instance: %ld sending a packet to a non existant UE ID:RAB: %lu/%x\n", instance, ue_id, bearer_id);
    pthread_mutex_unlock(&globGtp.gtp_lock);
    return;
  }

  extensionHeader_t *extensionHeader;
  extensionHeader = (extensionHeader_t *) calloc(1, sizeof(extensionHeader_t));
  fillDlDeliveryStatusReport(extensionHeader, req->buffer_availability,0);

  LOG_I(GTPU,"[%ld] GTP-U sending DL Data Delivery status to UE ID:RAB:teid %lu/%x/%x, oldseq %d, oldnum %d\n",
        instance, ue_id, bearer_id,ptr2->second.teid_outgoing, ptr2->second.seqNum,ptr2->second.npduNum );
  // copy to release the mutex
  gtpv1u_bearer_t tmp=ptr2->second;
  pthread_mutex_unlock(&globGtp.gtp_lock);
  gtpv1uCreateAndSendMsg(
      compatInst(instance), tmp.outgoing_ip_addr, tmp.outgoing_port, GTP_GPDU, tmp.teid_outgoing, NULL, 0, false, false, 0, 0, NR_RAN_CONTAINER, extensionHeader->buffer, extensionHeader->length);
}

static void gtpv1uEndTunnel(instance_t instance, gtpv1u_tunnel_data_req_t *req) {
  ue_id_t ue_id=req->ue_id;
  int  bearer_id=req->bearer_id;
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetVoid(compatInst(instance));
  getUeRetVoid(inst, ue_id);

  auto ptr2=ptrUe->second.bearers.find(bearer_id);

  if ( ptr2 == ptrUe->second.bearers.end() ) {
    LOG_E(GTPU,"[%ld] GTP-U sending a packet to a non existant UE:RAB: %lx/%x\n", instance, ue_id, bearer_id);
    pthread_mutex_unlock(&globGtp.gtp_lock);
    return;
  }

  LOG_D(GTPU,"[%ld] sending a end packet packet to UE:RAB:teid %lx/%x/%x\n",
        instance, ue_id, bearer_id,ptr2->second.teid_outgoing);
  gtpv1u_bearer_t tmp=ptr2->second;
  pthread_mutex_unlock(&globGtp.gtp_lock);
  Gtpv1uMsgHeaderT  msgHdr;
  // N should be 0 for us (it was used only in 2G and 3G)
  msgHdr.PN=0;
  msgHdr.S=0;
  msgHdr.E=0;
  msgHdr.spare=0;
  //PT=0 is for GTP' TS 32.295 (charging)
  msgHdr.PT=1;
  msgHdr.version=1;
  msgHdr.msgType=GTP_END_MARKER;
  msgHdr.msgLength=htons(0);
  msgHdr.teid=htonl(tmp.teid_outgoing);
  // Fix me: add IPv6 support, using flag ipVersion
  static struct sockaddr_in to= {0};
  to.sin_family      = AF_INET;
  to.sin_port        = htons(tmp.outgoing_port);
  to.sin_addr.s_addr = tmp.outgoing_ip_addr;
  char ip4[INET_ADDRSTRLEN];
  //char ip6[INET6_ADDRSTRLEN];
  LOG_D(GTPU,"[%ld] sending end packet to %s\n", instance, inet_ntoa(to.sin_addr) );

  if (sendto(compatInst(instance), (void *)&msgHdr, sizeof(msgHdr), 0,(struct sockaddr *)&to, sizeof(to) ) !=  sizeof(msgHdr)) {
    LOG_E(GTPU,
          "[%ld] Failed to send data to %s on port %d, buffer size %lu\n",
          compatInst(instance), inet_ntop(AF_INET, &tmp.outgoing_ip_addr, ip4, INET_ADDRSTRLEN), tmp.outgoing_port, sizeof(msgHdr));
  }
}

static  int udpServerSocket(openAddr_s addr) {
  LOG_I(GTPU, "Initializing UDP for local address %s with port %s\n", addr.originHost, addr.originService);
  int status;
  struct addrinfo hints= {0}, *servinfo, *p;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  if ((status = getaddrinfo(addr.originHost, addr.originService, &hints, &servinfo)) != 0) {
    LOG_E(GTPU,"getaddrinfo error: %s\n", gai_strerror(status));
    return -1;
  }

  int sockfd=-1;

  // loop through all the results and bind to the first we can
  for(p = servinfo; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype,
                         p->ai_protocol)) == -1) {
      LOG_W(GTPU,"socket: %s\n", strerror(errno));
      continue;
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      LOG_W(GTPU,"bind: %s\n", strerror(errno));
      continue;
    } else {
      // We create the gtp instance on the socket
      globGtp.instances[sockfd].addr=addr;

      if (p->ai_family == AF_INET) {
        struct sockaddr_in *ipv4=(struct sockaddr_in *)p->ai_addr;
        memcpy(globGtp.instances[sockfd].foundAddr,
               &ipv4->sin_addr.s_addr, sizeof(ipv4->sin_addr.s_addr));
        globGtp.instances[sockfd].foundAddrLen=sizeof(ipv4->sin_addr.s_addr);
        globGtp.instances[sockfd].ipVersion=4;
        break;
      } else if (p->ai_family == AF_INET6) {
        LOG_W(GTPU,"Local address is IP v6\n");
        struct sockaddr_in6 *ipv6=(struct sockaddr_in6 *)p->ai_addr;
        memcpy(globGtp.instances[sockfd].foundAddr,
               &ipv6->sin6_addr.s6_addr, sizeof(ipv6->sin6_addr.s6_addr));
        globGtp.instances[sockfd].foundAddrLen=sizeof(ipv6->sin6_addr.s6_addr);
        globGtp.instances[sockfd].ipVersion=6;
      } else
        AssertFatal(false,"Local address is not IPv4 or IPv6");
    }

    break; // if we get here, we must have connected successfully
  }

  if (p == NULL) {
    // looped off the end of the list with no successful bind
    LOG_E(GTPU,"failed to bind socket: %s %s \n", addr.originHost, addr.originService);
    return -1;
  }

  freeaddrinfo(servinfo); // all done with this structure

  if (strlen(addr.destinationHost)>1) {
    struct addrinfo hints;
    memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_DGRAM;
    hints.ai_protocol=0;
    hints.ai_flags=AI_PASSIVE|AI_ADDRCONFIG;
    struct addrinfo *res=0;
    int err=getaddrinfo(addr.destinationHost,addr.destinationService,&hints,&res);

    if (err==0) {
      for(p = res; p != NULL; p = p->ai_next) {
        if ((err=connect(sockfd,  p->ai_addr, p->ai_addrlen))==0)
          break;
      }
    }

    if (err)
      LOG_E(GTPU,"Can't filter remote host: %s, %s\n", addr.destinationHost,addr.destinationService);
  }

  int sendbuff = 1000*1000*10;
  AssertFatal(0==setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sendbuff, sizeof(sendbuff)),"");
  LOG_D(GTPU,"[%d] Created listener for paquets to: %s:%s, send buffer size: %d\n", sockfd, addr.originHost, addr.originService,sendbuff);
  return sockfd;
}

void remove_whitespace(char *str) {
    int len = strlen(str);
    int j = 0;

    for (int i = 0; i < len; i++) {
        if (str[i] != ' ' && str[i] != '\t' && str[i] != '\n' && str[i] != '\r') {
            str[j++] = str[i];
        }
    }
    str[j] = '\0'; // ²K¥[¦r²Å¦ê?§ô²Å
}

void extract_json_value(char *json, const char *key, void *dest) {
    char *start = strstr(json, key);
    if (start != NULL) {
        start += strlen(key) + 1; // Move to the start of the value
        sscanf(start, "%[^,\n}]", (char *)dest);
    }
}

static int
device_info_process(char *json_data)
{
  //struct device_table_entry *entry;
  //uint8_t device_mac_addr[6];

  //char *json_string = "{\"device_mac_addr\": [\"08\", \"00\", \"27\", \"24\", \"92\", \"CA\"], \"cpe_ip\": 16792586, \"device_ip\": 167880896}";
  printf("JSON string:%s", json_data);
  //printf("JSON string:%s", json_string);
  remove_whitespace(json_data);
  printf("JSON string:%s", json_data);
  //char cleaned_data[256];
  //strcpy(cleaned_data, json_data);
  //remove_whitespace(cleaned_data);
  // const char *mac_addr_start = strstr(json_data, "device_mac_addr: [");
  // if (mac_addr_start == NULL) {
  //   printf("Error: \"device_mac_addr\" not found in JSON data!\n");
  //   return -1;
  // }
  // const char *mac_addr_values = mac_addr_start + strlen("device_mac_addr: [");
  // char *token;
  // for (int i = 0; i < 6; i++) {
  //   token = strtok((i == 0) ? (char *)mac_addr_values : NULL, ", ");
  //   if (token == NULL) {
  //     printf("Error: Unexpected end of JSON data!\n");
  //     return -1;
  //   }
  //   entry->device_mac_addr[i] = strtol(token, NULL, 16);
  // }
  // const char *cpe_ip_start = strstr(json_data, "\"cpe_ip\": ");
  // const char *device_ip_start = strstr(json_data, "\"device_ip\": ");
  // if (cpe_ip_start == NULL || device_ip_start == NULL) {
  //   printf("Error: \"cpe_ip\" or \"device_ip\" not found in JSON data!\n");
  //   return -1;
  // }

    // ´£¨ú "cpe_ip" ©M "device_ip" ªº­È¦}???¾ã?
  // entry->cpe_ip = atoi(cpe_ip_start + strlen("\"cpe_ip\": "));
  // entry->device_ip = atoi(device_ip_start + strlen("\"device_ip\": "));
  printf("ok1\n");
  uint8_t mac_addr[6];
  uint32_t cpe_ip;
  uint32_t device_ip;
  //extract_json_value(json_data, "\"device_mac_addr\":[", mac_addr);

    // Extract CPE IP
  //extract_json_value(json_data, "\"cpe_ip\":", &cpe_ip);

    // Extract device IP
  //extract_json_value(json_data, "\"device_ip\":", &device_ip);

  sscanf(json_data, "{\"device_mac_addr\":[\"%2hhx\", \"%2hhx\", \"%2hhx\", \"%2hhx\", \"%2hhx\", \"%2hhx\"], \"cpe_ip\": %u, \"device_ip\": %u}",
         &mac_addr[0], &mac_addr[1], &mac_addr[2], &mac_addr[3], &mac_addr[4], &mac_addr[5], &cpe_ip, &device_ip);
  printf("ok2\n");
  // ÌÛ«Ø??ªí?¥Ø
  //struct device_table_entry entry;
  // for (int i = 0; i < 6; ++i) {
  //   entry->device_mac_addr[i] = mac_addr[i];
  // }
  //memcpy(entry->device_mac_addr, mac_addr, 6);
  //entry->cpe_ip = cpe_ip;
  //entry->device_ip = device_ip;
  printf("ok3\n");
  // ¥´¦L??ªí?¥Ø«H®§
  printf("Device MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", mac_addr[0], mac_addr[1],
           mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  printf("CPE IP: %u\n", cpe_ip);
  printf("Device IP: %u\n", device_ip);
  //sscanf(json_data, "{\"device_mac_addr\":[\"%hhx\",\"%hhx\",\"%hhx\",\"%hhx\",\"%hhx\",\"%hhx\"]",
  //          &entry->device_mac_addr[0], &entry->device_mac_addr[1], &entry->device_mac_addr[2],
  //          &entry->device_mac_addr[3], &entry->device_mac_addr[4], &entry->device_mac_addr[5]);
  
  
  //sscanf(json_data, "\"cpe_ip\":%u", &entry->cpe_ip);

  
  //sscanf(json_data, "\"device_ip\":%u", &entry->device_ip);

  //printf("cpe ip: %u\n", &entry->cpe_ip);
  //printf("device ip: %u\n", &entry->device_ip);
  //printf("he0\n");
  device_table_insert(mac_addr, cpe_ip, device_ip);
  //printf("he1\n");
  device_arp_table_insert(mac_addr, device_ip);
  // cJSON *json = cJSON_Parse(json_data);
  // if (json == NULL) {
  //   printf("Error parsing JSON: %s\n", cJSON_GetErrorPtr());
  // }

  // cJSON *mac_addr = cJSON_GetObjectItem(json, "device_mac_addr");
  // cJSON *cpe_ip = cJSON_GetObjectItem(json, "cpe_ip");
  // cJSON *device_ip = cJSON_GetObjectItem(json, "device_ip");
  // //printf("cpe ip: %u\n", (uint32_t)cpe_ip->valueint);

  // if (mac_addr != NULL && mac_addr->type == cJSON_Array && cJSON_GetArraySize(mac_addr) == 6 &&
  //   cpe_ip != NULL && cpe_ip->type == cJSON_Number &&
  //   device_ip != NULL && device_ip->type == cJSON_Number) {

  //   for (int i = 0; i < 6; i++) {
  //     device_mac_addr[i] = (uint8_t)strtol(cJSON_GetArrayItem(mac_addr, i)->valuestring, NULL, 16);
  //   }

  //   printf("he0\n");
  //   device_table_insert(device_mac_addr, (uint32_t)cpe_ip->valueint, (uint32_t)device_ip->valueint);
  //   printf("he1\n");
  //   device_arp_table_insert(device_mac_addr, (uint32_t)device_ip->valueint);
  // }
  // else {
  //   printf("Error: JSON format is not correct.\n");
  // }

  // cJSON_Delete(json);

  return -1;
}

void *loop1(void *arg) {
  unsigned char buffer[BUFFER_SIZE];
  while (1) {
    int len = recv(rawsockfd, buffer, BUFFER_SIZE, 0);
    if (len == -1) {
      perror("recv");
      continue;
    }
    else{
      printf("packet from server\n");
      //dump_data(buffer, len);
    }
    struct ether_header *eth_header = (struct ether_header *)buffer;
    if (ntohs(eth_header->ether_type) == ETHERTYPE_IP) {
      //printf("IPv4 packet\n");
      struct ip *ip_hdr = (struct ip*)(buffer + sizeof(struct ether_header));
      uint32_t ip_addr = ip_hdr->ip_dst.s_addr;
      if (ip_hdr->ip_p == IPPROTO_GRE) {
        //printf("GRE packet\n");
        uint32_t subnet = 0x0A3C0063;
        uint32_t mask = 0xFFFFFFFF;
        uint32_t ip_src_addr = ip_hdr->ip_src.s_addr;
        uint32_t ip_dst_addr = ip_hdr->ip_dst.s_addr;
        //printf("ip: %u\n", ip_src_addr);
        gtpEndPoint * inst;
        gtpv1u_bearer_t tmp;
        if (ip_src_addr == 1660959754){ //IP=10.60.0.99
          //printf("is DHCP ack\n");
          unsigned char *packet = buffer;
          packet += sizeof(struct ether_header);
          len -= sizeof(struct ether_header);
          len += 3;
          uint32_t temp = ip_dst_addr % 1000000;
          //printf("temp: %u\n", temp);
          size_t prepend_length = sizeof(pdcp[temp]);
          memmove(packet + prepend_length, packet, len);
          memcpy(packet, pdcp[temp], prepend_length);
          pdcp[temp][2]++;
          if (pdcp[temp][2] == 0) {
            pdcp[temp][1]++;
          }
          //dump_data(packet, len);              
          auto instChk=globGtp.instances.find(compatInst(compatInst(temp_instance)));
          if (instChk == globGtp.instances.end()) {                        
            LOG_E(GTPU,"try to get a gtp-u not existing output\n");     
            pthread_mutex_unlock(&globGtp.gtp_lock);
            //printf("fuck\n");                
            return NULL;                                              
          }
          else{
            //printf("get instance\n");
            if (&instChk->second){
              gtpEndPoint * inst=&instChk->second;
              auto ptrUe=inst->ue2te_mapping.find(UE_CPE[temp]);                                                                    
              if ( ptrUe==inst->ue2te_mapping.end() ) {                          
                pthread_mutex_unlock(&globGtp.gtp_lock);
                //printf("fuck2\n");                        
                return NULL;                                                             
              }
              //printf("ue id\n");
              //getUeRetVoid(inst, 15729); //ue_id
              auto ptr2=ptrUe->second.bearers.find(1); //bearer_id
              tmp=ptr2->second;
              //printf("get bearer id\n");
              gtpv1uCreateAndSendMsg(compatInst(temp_instance),
                                 tmp.outgoing_ip_addr,
                                 tmp.outgoing_port,
                                 GTP_GPDU,
                                 tmp.teid_outgoing,
                                 packet,
                                 len, 
                                 0, 
                                 0, 
                                 0, 
                                 0, 
                                 NO_MORE_EXT_HDRS, 
                                 NULL, 
                                 0);
            }
            //printf("finish\n");
          }                                                        
        }
      }
      else if (ip_hdr->ip_p == IPPROTO_UDP){ //process UDP packet
        struct udphdr *udp_hdr = (struct udphdr*)(buffer + sizeof(struct ether_header) + sizeof(struct ip));
        //process Device Info
        if (ntohs(udp_hdr->dest) == 5555){
          size_t json_offset = sizeof(struct ether_header) + sizeof(struct ip) + sizeof(struct udphdr);
          //size_t udp_packet_len = ntohs(udp_hdr->len);
          //size_t json_len = udp_packet_len - sizeof(struct udphdr);
          char *json_data = (char *)buffer + json_offset;
          printf("Device info get\n");
          device_info_process(json_data);
        }
      }
    }
  }
  return NULL;
}

instance_t gtpv1Init(openAddr_t context) {
  pthread_mutex_lock(&globGtp.gtp_lock);
  char cu_ip[] = "192.168.31.100";
  int result1 = strncmp(context.originHost, cu_ip, 14);
  int id=udpServerSocket(context);

  if (id>=0) {
    itti_subscribe_event_fd(TASK_GTPV1_U, id);
  } else
    LOG_E(GTPU,"can't create GTP-U instance\n");

  if(result1 == 0){
    createrawsocket();
    initialize_pdcp_array();
    printf("suitable\n");
    temp_instance = id;
    pthread_t tid1;
    if (pthread_create(&tid1, NULL, loop1, NULL) != 0) {
      perror("pthread_create");
    }
  }
  pthread_mutex_unlock(&globGtp.gtp_lock);

  LOG_I(GTPU, "Created gtpu instance id: %d\n", id);
  return id;
}

void GtpuUpdateTunnelOutgoingAddressAndTeid(instance_t instance, ue_id_t ue_id, ebi_t bearer_id, in_addr_t newOutgoingAddr, teid_t newOutgoingTeid) {
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetVoid(compatInst(instance));
  getUeRetVoid(inst, ue_id);

  auto ptr2=ptrUe->second.bearers.find(bearer_id);

  if ( ptr2 == ptrUe->second.bearers.end() ) {
    LOG_E(GTPU,"[%ld] Update tunnel for a existing ue id %lu, but wrong bearer_id %u\n", instance, ue_id, bearer_id);
    pthread_mutex_unlock(&globGtp.gtp_lock);
    return;
  }

  ptr2->second.outgoing_ip_addr = newOutgoingAddr;
  ptr2->second.teid_outgoing = newOutgoingTeid;
  LOG_I(GTPU, "[%ld] Tunnel Outgoing TEID updated to %x and address to %x\n", instance, ptr2->second.teid_outgoing, ptr2->second.outgoing_ip_addr);
  pthread_mutex_unlock(&globGtp.gtp_lock);
  return;
}

teid_t newGtpuCreateTunnel(instance_t instance,
                           ue_id_t ue_id,
                           int incoming_bearer_id,
                           int outgoing_bearer_id,
                           teid_t outgoing_teid,
                           int outgoing_qfi,
                           transport_layer_addr_t remoteAddr,
                           int port,
                           gtpCallback callBack,
                           gtpCallbackSDAP callBackSDAP) {
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetInt(compatInst(instance));
  auto it=inst->ue2te_mapping.find(ue_id);

  if ( it != inst->ue2te_mapping.end() &&  it->second.bearers.find(outgoing_bearer_id) != it->second.bearers.end()) {
    LOG_W(GTPU,"[%ld] Create a config for a already existing GTP tunnel (ue id %lu)\n", instance, ue_id);
    inst->ue2te_mapping.erase(it);
  }

  teid_t incoming_teid=gtpv1uNewTeid();

  while (globGtp.te2ue_mapping.find(incoming_teid) != globGtp.te2ue_mapping.end()) {
    LOG_W(GTPU, "[%ld] generated a random Teid that exists, re-generating (%x)\n", instance, incoming_teid);
    incoming_teid=gtpv1uNewTeid();
  };

  globGtp.te2ue_mapping[incoming_teid].ue_id = ue_id;
  globGtp.te2ue_mapping[incoming_teid].incoming_rb_id = incoming_bearer_id;
  globGtp.te2ue_mapping[incoming_teid].outgoing_teid = outgoing_teid;
  globGtp.te2ue_mapping[incoming_teid].callBack = callBack;
  globGtp.te2ue_mapping[incoming_teid].callBackSDAP = callBackSDAP;
  globGtp.te2ue_mapping[incoming_teid].pdusession_id = (uint8_t)outgoing_bearer_id;

  gtpv1u_bearer_t *tmp=&inst->ue2te_mapping[ue_id].bearers[outgoing_bearer_id];

  int addrs_length_in_bytes = remoteAddr.length / 8;

  switch (addrs_length_in_bytes) {
    case 4:
      memcpy(&tmp->outgoing_ip_addr,remoteAddr.buffer,4);
      break;

    case 16:
      memcpy(tmp->outgoing_ip6_addr.s6_addr,remoteAddr.buffer,
             16);
      break;

    case 20:
      memcpy(&tmp->outgoing_ip_addr,remoteAddr.buffer,4);
      memcpy(tmp->outgoing_ip6_addr.s6_addr,
             remoteAddr.buffer+4,
             16);

    default:
      AssertFatal(false, "SGW Address size impossible");
  }

  tmp->teid_incoming = incoming_teid;
  tmp->outgoing_port=port;
  tmp->teid_outgoing= outgoing_teid;
  tmp->outgoing_qfi=outgoing_qfi;
  pthread_mutex_unlock(&globGtp.gtp_lock);
  char ip4[INET_ADDRSTRLEN];
  char ip6[INET6_ADDRSTRLEN];
  LOG_I(GTPU,
        "[%ld] Created tunnel for UE ID %lu, teid for incoming: %x, teid for outgoing %x to remote IPv4: %s, IPv6 %s\n",
        instance,
        ue_id,
        tmp->teid_incoming,
        tmp->teid_outgoing,
        inet_ntop(AF_INET, (void *)&tmp->outgoing_ip_addr, ip4, INET_ADDRSTRLEN),
        inet_ntop(AF_INET6, (void *)&tmp->outgoing_ip6_addr.s6_addr, ip6, INET6_ADDRSTRLEN));
  return incoming_teid;
}

int gtpv1u_create_s1u_tunnel(instance_t instance,
                             const gtpv1u_enb_create_tunnel_req_t  *create_tunnel_req,
                             gtpv1u_enb_create_tunnel_resp_t *create_tunnel_resp,
                             gtpCallback callBack)
{
  LOG_D(GTPU, "[%ld] Start create tunnels for UE ID %u, num_tunnels %d, sgw_S1u_teid %x\n",
        instance,
        create_tunnel_req->rnti,
        create_tunnel_req->num_tunnels,
        create_tunnel_req->sgw_S1u_teid[0]);
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetInt(compatInst(instance));
  
  tcp_udp_port_t dstport=inst->get_dstport();
  uint8_t addr[inst->foundAddrLen];
  memcpy(addr, inst->foundAddr, inst->foundAddrLen);
  pthread_mutex_unlock(&globGtp.gtp_lock);
  
  for (int i = 0; i < create_tunnel_req->num_tunnels; i++) {
    AssertFatal(create_tunnel_req->eps_bearer_id[i] > 4,
                "From legacy code not clear, seems impossible (bearer=%d)\n",
                create_tunnel_req->eps_bearer_id[i]);
    int incoming_rb_id=create_tunnel_req->eps_bearer_id[i]-4;
    teid_t teid = newGtpuCreateTunnel(compatInst(instance),
                                      create_tunnel_req->rnti,
                                      incoming_rb_id,
                                      create_tunnel_req->eps_bearer_id[i],
                                      create_tunnel_req->sgw_S1u_teid[i],
                                      -1, // no pdu session in 4G
                                      create_tunnel_req->sgw_addr[i],
                                      dstport,
                                      callBack,
                                      NULL);
    create_tunnel_resp->status=0;
    create_tunnel_resp->rnti=create_tunnel_req->rnti;
    create_tunnel_resp->num_tunnels=create_tunnel_req->num_tunnels;
    create_tunnel_resp->enb_S1u_teid[i]=teid;
    create_tunnel_resp->eps_bearer_id[i] = create_tunnel_req->eps_bearer_id[i];
    memcpy(create_tunnel_resp->enb_addr.buffer,addr,sizeof(addr));
    create_tunnel_resp->enb_addr.length= sizeof(addr);
  }

  return !GTPNOK;
}

int gtpv1u_update_s1u_tunnel(
  const instance_t                              instance,
  const gtpv1u_enb_create_tunnel_req_t *const   create_tunnel_req,
  const rnti_t                                  prior_rnti
) {
  LOG_D(GTPU, "[%ld] Start update tunnels for old RNTI %x, new RNTI %x, num_tunnels %d, sgw_S1u_teid %x, eps_bearer_id %x\n",
        instance,
        prior_rnti,
        create_tunnel_req->rnti,
        create_tunnel_req->num_tunnels,
        create_tunnel_req->sgw_S1u_teid[0],
        create_tunnel_req->eps_bearer_id[0]);
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetInt(compatInst(instance));

  if ( inst->ue2te_mapping.find(create_tunnel_req->rnti) == inst->ue2te_mapping.end() ) {
    LOG_E(GTPU,"[%ld] Update not already existing tunnel (new rnti %x, old rnti %x)\n", 
          instance, create_tunnel_req->rnti, prior_rnti);
  }

  auto it=inst->ue2te_mapping.find(prior_rnti);

  if ( it != inst->ue2te_mapping.end() ) {
    pthread_mutex_unlock(&globGtp.gtp_lock);
    AssertFatal(false, "logic bug: update of non-existing tunnel (new ue id %u, old ue id %u)\n", create_tunnel_req->rnti, prior_rnti);
    /* we don't know if we need 4G or 5G PDCP and can therefore not create a
     * new tunnel */
    return 0;
  }

  inst->ue2te_mapping[create_tunnel_req->rnti]=it->second;
  inst->ue2te_mapping.erase(it);
  pthread_mutex_unlock(&globGtp.gtp_lock);
  return 0;
}

int gtpv1u_create_ngu_tunnel(const instance_t instance,
                             const gtpv1u_gnb_create_tunnel_req_t *const create_tunnel_req,
                             gtpv1u_gnb_create_tunnel_resp_t *const create_tunnel_resp,
                             gtpCallback callBack,
                             gtpCallbackSDAP callBackSDAP)
{
  LOG_D(GTPU, "[%ld] Start create tunnels for ue id %lu, num_tunnels %d, sgw_S1u_teid %x\n",
        instance,
        create_tunnel_req->ue_id,
        create_tunnel_req->num_tunnels,
        create_tunnel_req->outgoing_teid[0]);
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetInt(compatInst(instance));

  tcp_udp_port_t dstport = inst->get_dstport();
  uint8_t addr[inst->foundAddrLen];
  memcpy(addr, inst->foundAddr, inst->foundAddrLen);
  pthread_mutex_unlock(&globGtp.gtp_lock);
  for (int i = 0; i < create_tunnel_req->num_tunnels; i++) {
    teid_t teid = newGtpuCreateTunnel(instance,
                                      create_tunnel_req->ue_id,
                                      create_tunnel_req->incoming_rb_id[i],
                                      create_tunnel_req->pdusession_id[i],
                                      create_tunnel_req->outgoing_teid[i],
                                      create_tunnel_req->outgoing_qfi[i],
                                      create_tunnel_req->dst_addr[i],
                                      dstport,
                                      callBack,
                                      callBackSDAP);
    create_tunnel_resp->status=0;
    create_tunnel_resp->ue_id=create_tunnel_req->ue_id;
    create_tunnel_resp->num_tunnels=create_tunnel_req->num_tunnels;
    create_tunnel_resp->gnb_NGu_teid[i]=teid;
    memcpy(create_tunnel_resp->gnb_addr.buffer,addr,sizeof(addr));
    create_tunnel_resp->gnb_addr.length= sizeof(addr);
  }

  return !GTPNOK;
}

int gtpv1u_update_ngu_tunnel(const instance_t instanceP, const gtpv1u_gnb_create_tunnel_req_t *const create_tunnel_req_pP, const ue_id_t prior_ue_id)
{
  LOG_D(GTPU, "[%ld] Update tunnels from UEid %lx to UEid %lx\n", instanceP, prior_ue_id, create_tunnel_req_pP->ue_id);

  pthread_mutex_lock(&globGtp.gtp_lock);

  auto inst = &globGtp.instances[compatInst(instanceP)];
  auto it = inst->ue2te_mapping.find(prior_ue_id);
  if (it == inst->ue2te_mapping.end()) {
    LOG_W(GTPU, "[%ld] Delete GTP tunnels for UEid: %lx, but no tunnel exits\n", instanceP, prior_ue_id);
    pthread_mutex_unlock(&globGtp.gtp_lock);
    return GTPNOK;
  }

  for (int i = 0; i < create_tunnel_req_pP->num_tunnels; i++) {
    teid_t incoming_teid = inst->ue2te_mapping[prior_ue_id].bearers[create_tunnel_req_pP->pdusession_id[i]].teid_incoming;
    if (globGtp.te2ue_mapping[incoming_teid].ue_id == prior_ue_id) {
      globGtp.te2ue_mapping[incoming_teid].ue_id = create_tunnel_req_pP->ue_id;
    }
  }

  inst->ue2te_mapping[create_tunnel_req_pP->ue_id] = it->second;
  inst->ue2te_mapping.erase(it);

  pthread_mutex_unlock(&globGtp.gtp_lock);
  return !GTPNOK;
}

int gtpv1u_create_x2u_tunnel(
  const instance_t instanceP,
  const gtpv1u_enb_create_x2u_tunnel_req_t   *const create_tunnel_req_pP,
  gtpv1u_enb_create_x2u_tunnel_resp_t *const create_tunnel_resp_pP) {
  AssertFatal( false, "to be developped\n");
}

int newGtpuDeleteAllTunnels(instance_t instance, ue_id_t ue_id) {
  LOG_D(GTPU, "[%ld] Start delete tunnels for ue id %lu\n",
        instance, ue_id);
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetInt(compatInst(instance));
  getUeRetInt(inst, ue_id);

  int nb=0;

  for (auto j=ptrUe->second.bearers.begin();
       j!=ptrUe->second.bearers.end();
       ++j) {
    globGtp.te2ue_mapping.erase(j->second.teid_incoming);
    nb++;
  }

  inst->ue2te_mapping.erase(ptrUe);
  pthread_mutex_unlock(&globGtp.gtp_lock);
  LOG_I(GTPU, "[%ld] Deleted all tunnels for ue id %ld (%d tunnels deleted)\n", instance, ue_id, nb);
  return !GTPNOK;
}

// Legacy delete tunnel finish by deleting all the ue id
// so the list of bearer provided is only a design bug
int gtpv1u_delete_s1u_tunnel( const instance_t instance,
                              const gtpv1u_enb_delete_tunnel_req_t *const req_pP) {
  return  newGtpuDeleteAllTunnels(instance, req_pP->rnti);
}

int newGtpuDeleteTunnels(instance_t instance, ue_id_t ue_id, int nbTunnels, pdusessionid_t *pdusession_id) {
  LOG_D(GTPU, "[%ld] Start delete tunnels for ue id %lu\n",
        instance, ue_id);
  pthread_mutex_lock(&globGtp.gtp_lock);
  getInstRetInt(compatInst(instance));
  getUeRetInt(inst, ue_id);
  int nb=0;

  for (int i=0; i<nbTunnels; i++) {
    auto ptr2=ptrUe->second.bearers.find(pdusession_id[i]);

    if ( ptr2 == ptrUe->second.bearers.end() ) {
      LOG_E(GTPU,"[%ld] GTP-U instance: delete of not existing tunnel UE ID:RAB: %ld/%x\n", instance, ue_id, pdusession_id[i]);
    } else {
      globGtp.te2ue_mapping.erase(ptr2->second.teid_incoming);
      nb++;
    }
  }

  if (ptrUe->second.bearers.size() == 0 )
    // no tunnels on this ue id, erase the ue entry
    inst->ue2te_mapping.erase(ptrUe);

  pthread_mutex_unlock(&globGtp.gtp_lock);
  LOG_I(GTPU, "[%ld] Deleted all tunnels for ue id %lu (%d tunnels deleted)\n", instance, ue_id, nb);
  return !GTPNOK;
}

int gtpv1u_delete_x2u_tunnel( const instance_t instanceP,
                              const gtpv1u_enb_delete_tunnel_req_t *const req_pP) {
  LOG_E(GTPU,"x2 tunnel not implemented\n");
  return 0;
}

int gtpv1u_delete_ngu_tunnel( const instance_t instance,
                              gtpv1u_gnb_delete_tunnel_req_t *req) {
  return  newGtpuDeleteTunnels(instance, req->ue_id, req->num_pdusession, req->pdusession_id);
}

static int Gtpv1uHandleEchoReq(int h,
                               uint8_t *msgBuf,
                               uint32_t msgBufLen,
                               uint16_t peerPort,
                               uint32_t peerIp) {
  Gtpv1uMsgHeaderT      *msgHdr = (Gtpv1uMsgHeaderT *) msgBuf;

  if ( msgHdr->version != 1 ||  msgHdr->PT != 1 ) {
    LOG_E(GTPU, "[%d] Received a packet that is not GTP header\n", h);
    return GTPNOK;
  }

  if ( msgHdr->S != 1 ) {
    LOG_E(GTPU, "[%d] Received a echo request packet with no sequence number \n", h);
    return GTPNOK;
  }

  uint16_t seq=ntohs(*(uint16_t *)(msgHdr+1));
  LOG_D(GTPU, "[%d] Received a echo request, TEID: %d, seq: %hu\n", h, msgHdr->teid, seq);
  uint8_t recovery[2]= {14,0};
  return gtpv1uCreateAndSendMsg(h, peerIp, peerPort, GTP_ECHO_RSP, ntohl(msgHdr->teid), recovery, sizeof recovery, true, false, seq, 0, NO_MORE_EXT_HDRS, NULL, 0);
}

static int Gtpv1uHandleError(int h,
                             uint8_t *msgBuf,
                             uint32_t msgBufLen,
                             uint16_t peerPort,
                             uint32_t peerIp) {
  LOG_E(GTPU,"Hadle error to be dev\n");
  int rc = GTPNOK;
  return rc;
}

static int Gtpv1uHandleSupportedExt(int h,
                                    uint8_t *msgBuf,
                                    uint32_t msgBufLen,
                                    uint16_t peerPort,
                                    uint32_t peerIp) {
  LOG_E(GTPU,"Supported extensions to be dev\n");
  int rc = GTPNOK;
  return rc;
}

// When end marker arrives, we notify the client with buffer size = 0
// The client will likely call "delete tunnel"
// nevertheless we don't take the initiative
static int Gtpv1uHandleEndMarker(int h,
                                 uint8_t *msgBuf,
                                 uint32_t msgBufLen,
                                 uint16_t peerPort,
                                 uint32_t peerIp) {
  Gtpv1uMsgHeaderT      *msgHdr = (Gtpv1uMsgHeaderT *) msgBuf;

  if ( msgHdr->version != 1 ||  msgHdr->PT != 1 ) {
    LOG_E(GTPU, "[%d] Received a packet that is not GTP header\n", h);
    return GTPNOK;
  }

  pthread_mutex_lock(&globGtp.gtp_lock);
  // the socket Linux file handler is the instance id
  getInstRetInt(h);

  auto tunnel = globGtp.te2ue_mapping.find(ntohl(msgHdr->teid));

  if (tunnel == globGtp.te2ue_mapping.end()) {
    LOG_E(GTPU,"[%d] Received a incoming packet on unknown teid (%x) Dropping!\n", h, msgHdr->teid);
    pthread_mutex_unlock(&globGtp.gtp_lock);
    return GTPNOK;
  }

  // This context is not good for gtp
  // frame, ... has no meaning
  // manyother attributes may come from create tunnel
  protocol_ctxt_t ctxt;
  ctxt.module_id = 0;
  ctxt.enb_flag = 1;
  ctxt.instance = inst->addr.originInstance;
  ctxt.rntiMaybeUEid = tunnel->second.ue_id;
  ctxt.frame = 0;
  ctxt.subframe = 0;
  ctxt.eNB_index = 0;
  ctxt.brOption = 0;
  const srb_flag_t     srb_flag=SRB_FLAG_NO;
  const rb_id_t        rb_id=tunnel->second.incoming_rb_id;
  const mui_t          mui=RLC_MUI_UNDEFINED;
  const confirm_t      confirm=RLC_SDU_CONFIRM_NO;
  const pdcp_transmission_mode_t mode=PDCP_TRANSMISSION_MODE_DATA;
  const uint32_t sourceL2Id=0;
  const uint32_t destinationL2Id=0;
  pthread_mutex_unlock(&globGtp.gtp_lock);

  if ( !tunnel->second.callBack(&ctxt,
                                srb_flag,
                                rb_id,
                                mui,
                                confirm,
                                0,
                                NULL,
                                mode,
                                &sourceL2Id,
                                &destinationL2Id) )
    LOG_E(GTPU,"[%d] down layer refused incoming packet\n", h);

  LOG_D(GTPU,"[%d] Received END marker packet for: teid:%x\n", h, ntohl(msgHdr->teid));
  return !GTPNOK;
}

static int Gtpv1uHandleGpdu(int h,
                            uint8_t *msgBuf,
                            uint32_t msgBufLen,
                            uint16_t peerPort,
                            uint32_t peerIp) {
  Gtpv1uMsgHeaderT      *msgHdr = (Gtpv1uMsgHeaderT *) msgBuf;

  if ( msgHdr->version != 1 ||  msgHdr->PT != 1 ) {
    LOG_E(GTPU, "[%d] Received a packet that is not GTP header\n", h);
    return GTPNOK;
  }

  pthread_mutex_lock(&globGtp.gtp_lock);
  // the socket Linux file handler is the instance id
  getInstRetInt(h);
  auto tunnel = globGtp.te2ue_mapping.find(ntohl(msgHdr->teid));

  if (tunnel == globGtp.te2ue_mapping.end()) {
    LOG_E(GTPU,"[%d] Received a incoming packet on unknown teid (%x) Dropping!\n", h, ntohl(msgHdr->teid));
    pthread_mutex_unlock(&globGtp.gtp_lock);
    return GTPNOK;
  }

  /* see TS 29.281 5.1 */
  //Minimum length of GTP-U header if non of the optional fields are present
  unsigned int offset = sizeof(Gtpv1uMsgHeaderT);

  //dump_data(msgBuf, msgBufLen);

  int8_t qfi = -1;
  bool rqi = false;
  uint32_t NR_PDCP_PDU_SN = 0;

  /* if E, S, or PN is set then there are 4 more bytes of header */
  if( msgHdr->E ||  msgHdr->S ||msgHdr->PN)
    offset += 4;

  if (msgHdr->E) {
    int next_extension_header_type = msgBuf[offset - 1];
    int extension_header_length;

    while (next_extension_header_type != NO_MORE_EXT_HDRS) {
      extension_header_length = msgBuf[offset];
      switch (next_extension_header_type) {
        case PDU_SESSION_CONTAINER: {
    if (offset + sizeof(PDUSessionContainerT) > msgBufLen ) {
      LOG_E(GTPU, "gtp-u received header is malformed, ignore gtp packet\n");
      return GTPNOK;
    }
          PDUSessionContainerT *pdusession_cntr = (PDUSessionContainerT *)(msgBuf + offset + 1);
          qfi = pdusession_cntr->QFI;
          rqi = pdusession_cntr->Reflective_QoS_activation;
          break;
        }
        case NR_RAN_CONTAINER: {
    if (offset + 1 > msgBufLen ) {
      LOG_E(GTPU, "gtp-u received header is malformed, ignore gtp packet\n");
      return GTPNOK;
    }
          uint8_t PDU_type = (msgBuf[offset+1]>>4) & 0x0f;
          if (PDU_type == 0){ //DL USER Data Format
            int additional_offset = 6; //Additional offset capturing the first non-mandatory octet (TS 38.425, Figure 5.5.2.1-1)
            if(msgBuf[offset+1]>>2 & 0x1){ //DL Discard Blocks flag is present
              LOG_I(GTPU, "DL User Data: DL Discard Blocks handling not enabled\n"); 
              additional_offset = additional_offset + 9; //For the moment ignore
            }
            if(msgBuf[offset+1]>>1 & 0x1){ //DL Flush flag is present
              LOG_I(GTPU, "DL User Data: DL Flush handling not enabled\n");
              additional_offset = additional_offset + 3; //For the moment ignore
            }
            if((msgBuf[offset+2]>>3)& 0x1){ //"Report delivered" enabled (TS 38.425, 5.4)
              /*Store the NR PDCP PDU SN for which a delivery status report shall be generated once the
               *PDU gets forwarded to the lower layers*/
              //NR_PDCP_PDU_SN = msgBuf[offset+6] << 16 | msgBuf[offset+7] << 8 | msgBuf[offset+8];
              NR_PDCP_PDU_SN = msgBuf[offset+additional_offset] << 16 | msgBuf[offset+additional_offset+1] << 8 | msgBuf[offset+additional_offset+2]; 
              LOG_D(GTPU, " NR_PDCP_PDU_SN: %u \n",  NR_PDCP_PDU_SN);
            }
          }
          else{
            LOG_W(GTPU, "NR-RAN container type: %d not supported \n", PDU_type);
          }
          break;
        }
        default:
          LOG_W(GTPU, "unhandled extension 0x%2.2x, skipping\n", next_extension_header_type);
    break;
      }

      offset += extension_header_length * EXT_HDR_LNTH_OCTET_UNITS;
      if (offset > msgBufLen ) {
  LOG_E(GTPU, "gtp-u received header is malformed, ignore gtp packet\n");
  return GTPNOK;
      }
      next_extension_header_type = msgBuf[offset - 1];
    }
  }

  // This context is not good for gtp
  // frame, ... has no meaning
  // manyother attributes may come from create tunnel
  protocol_ctxt_t ctxt;
  ctxt.module_id = 0;
  ctxt.enb_flag = 1;
  ctxt.instance = inst->addr.originInstance;
  ctxt.rntiMaybeUEid = tunnel->second.ue_id;
  ctxt.frame = 0;
  ctxt.subframe = 0;
  ctxt.eNB_index = 0;
  ctxt.brOption = 0;
  const srb_flag_t     srb_flag=SRB_FLAG_NO;
  const rb_id_t        rb_id=tunnel->second.incoming_rb_id;
  const mui_t          mui=RLC_MUI_UNDEFINED;
  const confirm_t      confirm=RLC_SDU_CONFIRM_NO;
  const sdu_size_t sdu_buffer_size = msgBufLen - offset;
  unsigned char *const sdu_buffer=msgBuf+offset;
  const pdcp_transmission_mode_t mode=PDCP_TRANSMISSION_MODE_DATA;
  const uint32_t sourceL2Id=0;
  const uint32_t destinationL2Id=0;
  pthread_mutex_unlock(&globGtp.gtp_lock);

  if (sdu_buffer_size > 0) {
    if (qfi != -1 && tunnel->second.callBackSDAP) {
      if ( !tunnel->second.callBackSDAP(&ctxt,
                                        tunnel->second.ue_id,
                                        srb_flag,
                                        rb_id,
                                        mui,
                                        confirm,
                                        sdu_buffer_size,
                                        sdu_buffer,
                                        mode,
                                        &sourceL2Id,
                                        &destinationL2Id,
                                        qfi,
                                        rqi,
                                        tunnel->second.pdusession_id) )
        LOG_E(GTPU,"[%d] down layer refused incoming packet\n", h);
    } else {
      if ( !tunnel->second.callBack(&ctxt,
                                    srb_flag,
                                    rb_id,
                                    mui,
                                    confirm,
                                    sdu_buffer_size,
                                    sdu_buffer,
                                    mode,
                                    &sourceL2Id,
                                    &destinationL2Id) )
        LOG_E(GTPU,"[%d] down layer refused incoming packet\n", h);
    }
  }

  if(NR_PDCP_PDU_SN > 0 && NR_PDCP_PDU_SN %5 ==0){
    LOG_D (GTPU, "Create and send DL DATA Delivery status for the previously received PDU, NR_PDCP_PDU_SN: %u \n", NR_PDCP_PDU_SN);
    int rlc_tx_buffer_space = nr_rlc_get_available_tx_space(ctxt.rntiMaybeUEid, rb_id + 3);
    LOG_D(GTPU, "Available buffer size in RLC for Tx: %d \n", rlc_tx_buffer_space);
    /*Total size of DDD_status PDU = 1 octet to report extension header length
     * size of mandatory part + 3 octets for highest transmitted/delivered PDCP SN
     * 1 octet for padding + 1 octet for next extension header type,
     * according to TS 38.425: Fig. 5.5.2.2-1 and section 5.5.3.24*/
    extensionHeader_t *extensionHeader;
    extensionHeader = (extensionHeader_t *) calloc(1, sizeof(extensionHeader_t)) ;
    extensionHeader->buffer[0] = (1+sizeof(DlDataDeliveryStatus_flagsT)+3+1+1)/4;
    DlDataDeliveryStatus_flagsT DlDataDeliveryStatus;
    DlDataDeliveryStatus.deliveredPdcpSn = 0;
    DlDataDeliveryStatus.transmittedPdcpSn= 1; 
    DlDataDeliveryStatus.pduType = 1;
    DlDataDeliveryStatus.drbBufferSize = htonl(rlc_tx_buffer_space); //htonl(10000000); //hardcoded for now but normally we should extract it from RLC
    memcpy(extensionHeader->buffer+1, &DlDataDeliveryStatus, sizeof(DlDataDeliveryStatus_flagsT));
    uint8_t offset = sizeof(DlDataDeliveryStatus_flagsT)+1;

    extensionHeader->buffer[offset] =   (NR_PDCP_PDU_SN >> 16) & 0xff;
    extensionHeader->buffer[offset+1] = (NR_PDCP_PDU_SN >> 8) & 0xff;
    extensionHeader->buffer[offset+2] = NR_PDCP_PDU_SN & 0xff;
    LOG_D(GTPU, "Octets reporting NR_PDCP_PDU_SN, extensionHeader-> %u:%u:%u \n",
          extensionHeader->buffer[offset],
          extensionHeader->buffer[offset+1],
          extensionHeader->buffer[offset+2]);
    extensionHeader->buffer[offset+3] = 0x00; //Padding octet
    extensionHeader->buffer[offset+4] = 0x00; //No more extension headers
    /*Total size of DDD_status PDU = size of mandatory part +
     * 3 octets for highest transmitted/delivered PDCP SN +
     * 1 octet for padding + 1 octet for next extension header type,
     * according to TS 38.425: Fig. 5.5.2.2-1 and section 5.5.3.24*/
    extensionHeader->length  = 1+sizeof(DlDataDeliveryStatus_flagsT)+3+1+1;
    gtpv1uCreateAndSendMsg(
        h, peerIp, peerPort, GTP_GPDU, globGtp.te2ue_mapping[ntohl(msgHdr->teid)].outgoing_teid, NULL, 0, false, false, 0, 0, NR_RAN_CONTAINER, extensionHeader->buffer, extensionHeader->length);
  }

  LOG_D(GTPU,"[%d] Received a %d bytes packet for: teid:%x\n", h,
        msgBufLen-offset,
        ntohl(msgHdr->teid));
  return !GTPNOK;
}

void gtpv1uReceiver(int h) {
  uint8_t           udpData[65536];
  int               udpDataLen;
  socklen_t          from_len;
  struct sockaddr_in addr;
  from_len = (socklen_t)sizeof(struct sockaddr_in);

  if ((udpDataLen = recvfrom(h, udpData, sizeof(udpData), 0,
                             (struct sockaddr *)&addr, &from_len)) < 0) {
    LOG_E(GTPU, "[%d] Recvfrom failed (%s)\n", h, strerror(errno));
    return;
  } else if (udpDataLen == 0) {
    LOG_W(GTPU, "[%d] Recvfrom returned 0\n", h);
    return;
  } else {
    if ( udpDataLen < (int)sizeof(Gtpv1uMsgHeaderT) ) {
      LOG_W(GTPU, "[%d] received malformed gtp packet \n", h);
      return;
    }
    Gtpv1uMsgHeaderT* msg=(Gtpv1uMsgHeaderT*) udpData;
    if ( (int)(ntohs(msg->msgLength) + sizeof(Gtpv1uMsgHeaderT)) != udpDataLen ) {
      LOG_W(GTPU, "[%d] received malformed gtp packet length\n", h);
      return;
    }
    LOG_D(GTPU, "[%d] Received GTP data, msg type: %x\n", h, msg->msgType);
    switch(msg->msgType) {
      case GTP_ECHO_RSP:
        break;

      case GTP_ECHO_REQ:
        Gtpv1uHandleEchoReq( h, udpData, udpDataLen, htons(addr.sin_port), addr.sin_addr.s_addr);
        break;

      case GTP_ERROR_INDICATION:
        Gtpv1uHandleError( h, udpData, udpDataLen, htons(addr.sin_port), addr.sin_addr.s_addr);
        break;

      case GTP_SUPPORTED_EXTENSION_HEADER_INDICATION:
        Gtpv1uHandleSupportedExt( h, udpData, udpDataLen, htons(addr.sin_port), addr.sin_addr.s_addr);
        break;

      case GTP_END_MARKER:
        Gtpv1uHandleEndMarker( h, udpData, udpDataLen, htons(addr.sin_port), addr.sin_addr.s_addr);
        break;

      case GTP_GPDU:
        //printf("GTP received\n");
        Gtpv1uHandleGpdu( h, udpData, udpDataLen, htons(addr.sin_port), addr.sin_addr.s_addr);
        break;

      default:
        LOG_E(GTPU, "[%d] Received a GTP packet of unknown type: %d\n", h, msg->msgType);
        break;
    }
  }
}

#include <openair2/ENB_APP/enb_paramdef.h>

void *gtpv1uTask(void *args)  {
  while(1) {
    /* Trying to fetch a message from the message queue.
       If the queue is empty, this function will block till a
       message is sent to the task.
    */
    MessageDef *message_p = NULL;
    itti_receive_msg(TASK_GTPV1_U, &message_p);

    if (message_p != NULL ) {
      openAddr_t addr= {{0}};
      const instance_t myInstance = ITTI_MSG_DESTINATION_INSTANCE(message_p);
      //temp_instance = myInstance;
      const int msgType = ITTI_MSG_ID(message_p);
      LOG_D(GTPU, "GTP-U received %s for instance %ld\n", messages_info[msgType].name, myInstance);
      switch (msgType) {
          // DATA TO BE SENT TO UDP

        case GTPV1U_TUNNEL_DATA_REQ: {
          gtpv1uSend(compatInst(myInstance), &GTPV1U_TUNNEL_DATA_REQ(message_p), false, false);
        }
        break;

        case GTPV1U_DU_BUFFER_REPORT_REQ:{
          gtpv1uSendDlDeliveryStatus(compatInst(myInstance), &GTPV1U_DU_BUFFER_REPORT_REQ(message_p));
        }
        break;

        case TERMINATE_MESSAGE:
          break;

        case TIMER_HAS_EXPIRED:
          LOG_E(GTPU, "Received unexpected timer expired (no need of timers in this version) %s\n", ITTI_MSG_NAME(message_p));
          break;

        case GTPV1U_ENB_END_MARKER_REQ:
          gtpv1uEndTunnel(compatInst(myInstance), &GTPV1U_TUNNEL_DATA_REQ(message_p));
          itti_free(TASK_GTPV1_U, GTPV1U_TUNNEL_DATA_REQ(message_p).buffer);
          break;

        case GTPV1U_ENB_DATA_FORWARDING_REQ:
        case GTPV1U_ENB_DATA_FORWARDING_IND:
        case GTPV1U_ENB_END_MARKER_IND:
          LOG_E(GTPU, "to be developped %s\n", ITTI_MSG_NAME(message_p));
          abort();
          break;

        case GTPV1U_REQ:
          // to be dev: should be removed, to use API
          strcpy(addr.originHost, GTPV1U_REQ(message_p).localAddrStr);
          strcpy(addr.originService, GTPV1U_REQ(message_p).localPortStr);
          strcpy(addr.destinationService,addr.originService);
          AssertFatal((legacyInstanceMapping=gtpv1Init(addr))!=0,"Instance 0 reserved for legacy\n");
          break;

        default:
          LOG_E(GTPU, "Received unexpected message %s\n", ITTI_MSG_NAME(message_p));
          abort();
          break;
      }

      AssertFatal(EXIT_SUCCESS==itti_free(TASK_GTPV1_U, message_p), "Failed to free memory!\n");
    }

    struct epoll_event events[20];
    int nb_events = itti_get_events(TASK_GTPV1_U, events, 20);

    for (int i = 0; i < nb_events; i++)
      if ((events[i].events&EPOLLIN))
        gtpv1uReceiver(events[i].data.fd);
  }

  // pthread_join(tid1, NULL);

  return NULL;
}

#ifdef __cplusplus
}
#endif
