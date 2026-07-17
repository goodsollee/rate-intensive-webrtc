// cellular_emulator.bpf.c
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <linux/if_ether.h>
#include <bpf/bpf_endian.h>

#define TC_ACT_OK 0
#define MAX_PKT_SIZE 256  // Maximum number of packet bytes to capture

struct packet_data {
    __u32 pkt_len;           // Full packet length
    __u32 arrival_time_ms;   // Packet arrival time (in milliseconds)
    __u8 data[MAX_PKT_SIZE]; // Snapshot of packet data (first MAX_PKT_SIZE bytes)
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 512 * 1024);  // Adjust the ring buffer size if needed
} packet_ringbuf SEC(".maps");

/*
 * This eBPF program is attached to the egress hook.
 * It captures every outgoing packet on the interface and copies up to MAX_PKT_SIZE bytes
 * along with some metadata into the ring buffer for userspace processing.
 */
SEC("classifier/egress")
int cellular_emulator(struct __sk_buff *skb) {
    void *data = (void *)(long)skb->data;
    void *data_end = (void *)(long)skb->data_end;
    __u32 pkt_len = skb->len;

    // Get current time in milliseconds.
    __u32 arrival_time = (__u32)(bpf_ktime_get_ns() / 1000000);

    // Reserve space in the ring buffer.
    struct packet_data *pkt;
    pkt = bpf_ringbuf_reserve(&packet_ringbuf, sizeof(*pkt), 0);
    if (!pkt)
        return TC_ACT_OK;

    pkt->pkt_len = pkt_len;
    pkt->arrival_time_ms = arrival_time;

    // Determine how many bytes to copy.
    __u32 copy_size = pkt_len < MAX_PKT_SIZE ? pkt_len : MAX_PKT_SIZE;
    if (data + copy_size > data_end)
        copy_size = data_end - data;

    // Copy packet data into the ring buffer.
    bpf_probe_read(pkt->data, copy_size, data);

    bpf_ringbuf_submit(pkt, 0);

    return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";
