
#include <ntp_time.hpp>
#ifdef ESP_PLATFORM
#if __has_include(<Arduino.h>)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#else
#include <string.h>
#include <sys/param.h>
#include <esp_system.h>
#include <esp_event.h>

#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <lwip/ip4_addr.h>
#include <lwip/dns.h>
#include <esp_netif.h>
#endif
#ifdef ARDUINO
namespace arduino {
static WiFiUDP g_ntp_time_udp;
#else
namespace esp_idf {
static volatile int ntp_time_resolve_host_state = 0;
static ip_addr_t ntp_time_resolve_host_ip;
static void ntp_time_resolve_host_wait() {
    while(ntp_time_resolve_host_state!=0) {
        static int count = 0;
        while(count++<7) {
            vTaskDelay(5);
        }
    }
}
static void ntp_time_resolve_host_handler(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
    if(ipaddr!=nullptr) {
        ntp_time_resolve_host_state = 0;
        ntp_time_resolve_host_ip = *ipaddr;
    } else {
        IP_ADDR4( &ntp_time_resolve_host_ip, 0,0,0,0 );    
    }
}
static bool ntp_time_resolve_host(const char* hostname, ip_addr_t* out_address) {
    ntp_time_resolve_host_wait();
    ntp_time_resolve_host_state = 1;
    dns_gethostbyname(hostname,&ntp_time_resolve_host_ip,ntp_time_resolve_host_handler,nullptr);
    ntp_time_resolve_host_wait();
    *out_address = ntp_time_resolve_host_ip;
    if(out_address->u_addr.ip4.addr!=0) {
        return true;
    }
    return false;   
}
#endif
bool ntp_time::send_packet() {
#ifdef ARDUINO
    IPAddress address;
    WiFi.hostByName(domain,address);
    //NTP requests are to port 123
    g_ntp_time_udp.beginPacket(address, 123); 
    g_ntp_time_udp.write(m_packet_buffer, 48);
    g_ntp_time_udp.endPacket();
    
#else
    if(m_socket!=-1) {
        close(m_socket);
        m_socket = -1;
    }
    ip_addr_t addy;
    IP_ADDR4( &addy, 0,0,0,0 );
    if(!ntp_time_resolve_host(domain,&addy)) {
        puts("resolution failure");
        return false;
    }
    m_addr.sin_addr.s_addr = addy.u_addr.ip4.addr;
    m_addr.sin_family = AF_INET;
    m_addr.sin_port = htons(123);
    m_socket = socket(AF_INET,SOCK_DGRAM,0);
    if(m_socket<0) {
        m_socket = -1;
        puts("packet send failure");
        return false;
    }
    int flags = fcntl(m_socket, F_GETFL, 0);
    fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
    int retcode = sendto(m_socket, m_packet_buffer,sizeof(m_packet_buffer), 0,(sockaddr *)&m_addr, sizeof(m_addr));
    if (retcode < 0)
    {
        close(m_socket);
        m_socket = -1;
        puts("packet send failure");
        return false;
    }
#endif
    puts("sent packet");
    return true;
}
bool ntp_time::begin_request(size_t retry_count,
                            uint32_t retry_ms,
                            ntp_time_callback callback, 
                            void* callback_state) {
    memset(m_packet_buffer, 0, 48);
    m_packet_buffer[0] = 0b11100011;   // LI, Version, Mode
    m_packet_buffer[1] = 0;     // Stratum, or type of clock
    m_packet_buffer[2] = 6;     // Polling Interval
    m_packet_buffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    m_packet_buffer[12]  = 49;
    m_packet_buffer[13]  = 0x4E;
    m_packet_buffer[14]  = 49;
    m_packet_buffer[15]  = 52;
    
    m_request_result = 0;
    m_retries = 0;
    m_retry_ts = 0;
    m_retry_count = retry_count;
    m_retry_timeout = retry_ms;
    m_callback_state = callback_state;
    m_callback = callback;
    if(!send_packet()) {
        return false;
    }
    m_requesting = true;
    return true;
}

bool ntp_time::update() {
    m_request_result = 0;
        
    if(m_requesting && millis()>m_retry_ts+m_retry_timeout) {
        m_retry_ts = millis();
        ++m_retries;
        if(m_retry_count>0 && m_retries>m_retry_count) {
            m_requesting = false;
            if(m_callback!=nullptr) {
                m_callback(m_callback_state);
            }
            m_retries = 0;
            m_retry_ts = 0;
            m_requesting = false;
            return false;
        }
        if(!send_packet()) {
           return false;
        }
    }
    bool result = false;
#ifdef ARDUINO
    // read the packet into the buffer
    // if we got a packet from NTP, read it
    if (0 < g_ntp_time_udp.parsePacket()) {
        g_ntp_time_udp.read(m_packet_buffer, 48); 

        result = true;
    }
#else
    if(m_socket<0) {
        return false;
    }
    socklen_t from_len = sizeof(m_packet_buffer);
    int retcode = recvfrom(m_socket, m_packet_buffer, sizeof(m_packet_buffer), 0,
      (struct sockaddr *)&m_addr,&from_len);
    if(retcode>0) {
        close(m_socket);
        m_socket = -1;
        result = true;
    }
#endif
    if(result) {
        //the timestamp starts at byte 40 of the received packet and is four bytes,
        // or two words, long. First, extract the two words:

        unsigned long hi = (m_packet_buffer[40] << 8) | m_packet_buffer[41];
        unsigned long lo = (m_packet_buffer[42] << 8) | m_packet_buffer[43];
        // combine the four bytes (two words) into a long integer
        // this is NTP time (seconds since Jan 1 1900):
        unsigned long since1900 = hi << 16 | lo;
        // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
        constexpr const unsigned long seventyYears = 2208988800UL;
        // subtract seventy years:
        m_request_result = since1900 - seventyYears;
        m_requesting = false;
        m_retries = 0;
        m_retry_ts = 0;
        if(m_callback!=nullptr) {
            m_callback(m_callback_state);
        }
    }
    return true;
}
}
#endif