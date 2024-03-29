#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pcap.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <string.h>

#define SNAP_LEN 1518
#define SIZE_ETHERNET 14
#define ETHER_ADDR_LEN 6
#define SYN_SYNACK_RATIO 3
#define MAX_ALLOWED_ATTEMPS 15
#define MAX_NUMBER_OF_SUSPECTS 1000

/* IP header */

struct sniff_ip {
    u_char ip_vhl;                      /* version << 4 | header length >> 2 */
    u_char ip_tos;                      /* type of service */
    u_short ip_len;                     /* total length */
    u_short ip_id;                      /* identification */
    u_short ip_off;                     /* fragment offset field */
    #define IP_RF 0x8000                /* reserved fragment flag */
    #define IP_DF 0x4000                /* dont fragment flag */
    #define IP_MF 0x2000                /* more fragments flag */
    #define IP_OFFMASK 0x1fff           /* mask for fragmenting bits */
    u_char ip_ttl;                      /* time to live */
    u_char ip_p;                        /* protocol */
    u_short ip_sum;                     /* checksum */
    struct in_addr ip_src, ip_dst;      /* source and dest address */
};

#define IP_HL(ip)               (((ip)->ip_vhl) & 0x0f)
#define IP_V(ip)                (((ip)->ip_vhl) >> 4)

/* TCP header */
typedef u_int tcp_seq;

struct sniff_tcp {
    u_short th_sport;                   /* source port */
    u_short th_dport;                   /* destination port */
    tcp_seq th_seq;                     /* sequence number */
    tcp_seq th_ack;                     /* acknowledgement number */
    u_char th_offx2;                    /* data offset, rsvd */
    #define TH_OFF(th)      (((th)->th_offx2 & 0xf0) >> 4)
    u_char th_flags;
    #define TH_FIN  0x01
    #define TH_SYN  0x02
    #define TH_RST  0x04
    #define TH_PUSH 0x08
    #define TH_ACK  0x10
    #define TH_SYN_ACK  0x12
    #define TH_RST_ACK  0x14
    #define TH_URG  0x20
    #define TH_ECE  0x40
    #define TH_CWR  0x80
    #define TH_FLAGS        (TH_FIN|TH_SYN|TH_RST|TH_ACK|TH_URG|TH_ECE|TH_CWR)
    u_short th_win;                     // window
    u_short th_sum;                     // checksum
    u_short th_urp;                     // urgent pointer
};

struct suspect {
    char* ip;
    int n_ack;
    int n_syn_ack;
    int syn;
    int prev_syn;
};

void got_packet(u_char*, const struct pcap_pkthdr*, const u_char*);
void find_and_increase(char*, int, int);
int cmp(const void*, const void*);
void add_or_clear_and_add(struct suspect*);
void print_suspects(int);

void detect();
void* checkSyn(void*);
void find_diff_in_syn();
void find_diff_in_syn_and_syn_ack_diff();
void log_ip(char*, int);
void log_helper(char*, int, int);

FILE* log_file;

struct suspect* suspects[MAX_NUMBER_OF_SUSPECTS];
int count = 0;
int syn_count = 0;


int
start_sniffing() {
    char* dev = NULL;     /* capture device name */
    char errbuf[PCAP_ERRBUF_SIZE];     /* error buffer */
    pcap_t* handle;     /* packet capture handle */

    struct bpf_program fp;     /* compiled filter program (expression) */
    bpf_u_int32 mask;     /* subnet mask */
    bpf_u_int32 net;     /* ip */


    // Find capture device.
    dev = pcap_lookupdev(errbuf);
    if (dev == NULL) {
        fprintf(stderr, "Couldn't find default device: %s\n", errbuf);
        exit(EXIT_FAILURE);
    }

    if (pcap_lookupnet(dev, &net, &mask, errbuf) == -1) {
        fprintf(stderr, "Couldn't get netmask for device %s: %s\n", dev, errbuf);
        net = 0;
        mask = 0;
    }

    handle = pcap_open_live(dev, SNAP_LEN, 1, 1000, errbuf);
    if (handle == NULL) {
        fprintf(stderr, "Couldn't open device %s: %s\n", dev, errbuf);
        exit(EXIT_FAILURE);
    }

    // Control if we are capturing on an Ethernet device.
    if (pcap_datalink(handle) != DLT_EN10MB) {
        fprintf(stderr, "%s is not an Ethernet\n", dev);
        exit(EXIT_FAILURE);
    }

    int c = 1;
    detect();

    while(1) {
        int failed = pcap_loop(handle, 1, got_packet, NULL);
        if (failed) exit(1);
        c++;
    }

    /* cleanup */
    pcap_freecode(&fp);
    pcap_close(handle);

    return 0;
}


void
got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet) {
    const struct sniff_ip *ip = (struct sniff_ip*)(packet + SIZE_ETHERNET);
    int size_ip = IP_HL(ip)*4;
    const struct sniff_tcp *tcp = (struct sniff_tcp*)(packet + SIZE_ETHERNET + size_ip);

    char* suspect_ip = inet_ntoa(ip->ip_src);
    switch (tcp->th_flags) {
    case TH_SYN:
        find_and_increase(suspect_ip, 0, 1);
        break;
    case TH_ACK:
        find_and_increase(inet_ntoa(ip->ip_dst), 1, 0);
        break;
    case TH_SYN_ACK:
        find_and_increase(inet_ntoa(ip->ip_dst), 0, 0);
        break;
    default:
        return;
    }

    return;
}

struct suspect*
create_suspect(char *ip) {
    struct suspect *s = malloc(sizeof(struct suspect));
    s->ip = strdup(ip);
    s->n_ack = 0;
    s->n_syn_ack = 0;
    s->syn = 0;
    s->prev_syn = 0;
    return s;
}

void
find_and_increase(char* suspect_ip, int is_ack, int is_syn) {
    if (is_syn) {
        syn_count++;
    }
    for (int i = 0; i < count; ++i) {
        char* ip = suspects[i]->ip;
        if (strcmp(ip, suspect_ip) == 0) {
            // We previously had this IP!
            is_ack ? suspects[i]->n_ack++ : (is_syn ? suspects[i]->syn++ : suspects[i]->n_syn_ack++);
            return;
        }
    }
    // We don't have this IP :(
    struct suspect* s = create_suspect(suspect_ip);
    is_ack ? s->n_ack++ : (is_syn ? s->syn++ : s->n_syn_ack++);
    add_or_clear_and_add(s);
    return;
}

void
add_or_clear_and_add(struct suspect* s) {
    if (count == MAX_NUMBER_OF_SUSPECTS - 1) {
        // Clear all but first 20.
        memset(suspects, 20, MAX_NUMBER_OF_SUSPECTS);
        suspects[21] = s;
        count = 20;
        return;
    }
    suspects[count++] = s;
}

int
cmp(const void *v1, const void *v2) {
    const struct suspect* p1 = *(struct suspect **)v1;
    const struct suspect* p2 = *(struct suspect **)v2;
    if (p1 && p2) {
        if (p1->syn < p2->syn)
            return +1;
        else if (p1->syn > p2->syn)
            return -1;
        else
            return 0;
    }
    return -1;
}

void
print_suspects(int hm) {
    qsort(suspects, count, sizeof(struct suspect*), &cmp);
    puts("================ PRINTING SUSPECTS ====================");
    for (size_t i = 0; i < hm; i++) {
        if (suspects[i]) {
            puts("---------------------------");
            printf("Suspect number: %zu\n", i);
            printf("IP: %s\n", suspects[i]->ip);
            printf("SYN Attemps: %d\n", suspects[i]->syn);
            printf("ACK Response: %d\n", suspects[i]->n_ack);
            printf("SYN-ACK Response: %d\n", suspects[i]->n_syn_ack);
            puts("---------------------------");
        }
    }
    puts("=======================================================\n\n\n\n\n");
}



/*------------------------DETECTION------------------------*/

void
detect() {
    pthread_t tid;
    if (pthread_create(&tid, NULL, &checkSyn, NULL) != 0) {
        puts("Can't create thread!");
        exit(EXIT_FAILURE);
    }
}

void*
checkSyn(void *arg) {
    int c = 0;
    while(1) {
        sleep(4);
        find_diff_in_syn();
        sleep(1);
        print_suspects(count);
        find_diff_in_syn_and_syn_ack_diff();
        c++;
    }
    pthread_exit(0);
}

char* reported_ip[MAX_NUMBER_OF_SUSPECTS];
int reported_count = 0;

int
contains(char* ip) {
    for (size_t i = 0; i < reported_count; ++i) {
        int eq = strcmp(reported_ip[reported_count], ip) == 0;
        if (eq) return 1;
    }
    return 0;
}

void
find_diff_in_syn() {
    for (size_t i = 0; i < count; i++) {
        if (suspects[i]) {
            char* ip = suspects[i]->ip;
            int syn = suspects[i]->syn;
            int prev_syn = suspects[i]->prev_syn;
            int diff = syn - prev_syn;
            if (!prev_syn && diff > 4) log_ip(ip, 1);
            suspects[i]->prev_syn = suspects[i]->syn;
        }
    }
}

void
find_diff_in_syn_and_syn_ack_diff() {
    for (size_t i = 0; i < count; i++) {
        if (suspects[i]) {
            char* ip = suspects[i]->ip;
            int syn = suspects[i]->syn;
            int syn_ack = suspects[i]->n_syn_ack;
            if (syn_ack) {
                int harmful_ratio = ceil(syn / syn_ack) > SYN_SYNACK_RATIO;
                if (harmful_ratio) log_ip(ip, 0);
            }
        }
    }
}

void
log_ip(char* ip, int sig) {
    int previously_reported = contains(ip);
    if (previously_reported) {
        log_helper(ip, 0, 0);
    } else {
        log_helper(ip, 1, sig);
    }
}

void
log_helper(char* ip, int is_single, int sig) {
    log_file = fopen("log.txt", "ab+");     /* 0444 log file */
    if (is_single) fprintf(log_file, "%s: %s\n", sig ? "SIG1" : "SIG2", ip);
    else fprintf(log_file, "SIG1 & SIG2: %s\n", ip);
    fclose(log_file);
}

static void
skeleton_daemon() {
    pid_t pid;
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) exit(EXIT_FAILURE);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    umask(0);
}

int
main() {
    // skeleton_daemon();
    // while(1) {
    start_sniffing();
    // }

    return EXIT_SUCCESS;
}
