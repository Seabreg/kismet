/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "tcpclient.h"
#include "networksort.h"

TcpClient::TcpClient() {
    sv_valid = 0;
    client_fd = 0;

    lat = lon = alt = spd = 0;
    mode = 0;

    num_networks = num_packets = num_crypt = num_interesting =
        num_noise = num_dropped = 0;

    start_time = 0;
    major = minor = tiny = 0;

    power = quality = noise = 0;

    maxstrings = 100;
    maxpackinfos = 1000;

    memset(status, 0, STATUS_MAX);
    memset(channel_graph, 0, sizeof(channel_power) * CHANNEL_MAX);

}

TcpClient::~TcpClient() {
    if (sv_valid)
        close(client_fd);

    sv_valid = 0;
}

int TcpClient::Connect(short int in_port, char *in_host) {
    // Copy the port to our local data
    port = in_port;

    // Resolve the hostname we were given/found to see if it's actually
    // valid
    if ((client_host = gethostbyname(in_host)) == NULL) {
        snprintf(errstr, 1024, "TcpClient could not resolve host \"%s\"\n", in_host);
        return (-1);
    }

    strncpy(hostname, in_host, MAXHOSTNAMELEN);

    // Set up our socket
    bzero(&client_sock, sizeof(client_sock));
    client_sock.sin_family = client_host->h_addrtype;
    memcpy((char *) &client_sock.sin_addr.s_addr, client_host->h_addr_list[0],
           client_host->h_length);
    client_sock.sin_port = htons(in_port);


    // Debug("Server::Setup calling socket()");
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        snprintf(errstr, 1024, "TcpClient socket() failed %d (%s)\n",
                 errno, sys_errlist[errno]);
        return (-2);
    }

    // Bind to the local half of the pair
    local_sock.sin_family = AF_INET;
    local_sock.sin_addr.s_addr = htonl(INADDR_ANY);
    local_sock.sin_port = htons(0);

    if (bind(client_fd, (struct sockaddr *) &local_sock, sizeof(local_sock)) < 0) {
        snprintf(errstr, 1024, "FATAL: TcpClient bind() failed %d (%s)\n",
                 errno, sys_errlist[errno]);
        return (-3);
    }

    // Connect
    if (connect(client_fd, (struct sockaddr *) &client_sock, sizeof(client_sock)) < 0) {
        snprintf(errstr, 1024, "FATAL: TcpClient connect() failed %d (%s)\n",
                 errno, sys_errlist[errno]);
        return (-4);
    }

    int save_mode = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, save_mode | O_NONBLOCK);

    sv_valid = 1;

    clientf = fdopen(client_fd, "r");

    return 1;
}

int TcpClient::Poll() {

    if (!sv_valid) {
        snprintf(errstr, 1024, "TcpClient poll() on an inactive connection.");
        return -1;
    }

    // Clear the status
    memset(status, 0, STATUS_MAX);

    int selected;
    fd_set read_set;
    fd_set write_set;
    fd_set except_set;

    FD_ZERO(&read_set);
    FD_SET(client_fd, &read_set);
    FD_ZERO(&write_set);
    FD_SET(client_fd, &write_set);
    FD_ZERO(&except_set);

    struct timeval tim;

    tim.tv_sec = 0;
    tim.tv_usec = 0;

    // Enter the select loop
    //Debug("Server::Poll() - Calling select()");
    if ((selected = select(client_fd+1, &read_set, &write_set, &except_set, &tim)) < 0) {
        if (errno != EINTR) {
            snprintf(errstr, 1024, "TcpServer select() returned %d (%s)\n",
                     errno, sys_errlist[errno]);
            sv_valid = 0;
            close(client_fd);
            return (-1);
        }
    }

    if (FD_ISSET(client_fd, &except_set)) {
        snprintf(errstr, 1024, "Exception on socket.\n");
        sv_valid = 0;
        close(client_fd);
        return(-1);
    }

    if (writebuf.length() > 0 && FD_ISSET(client_fd, &write_set)) {
        int res = write(client_fd, writebuf.c_str(), writebuf.length());

        if (res <= 0) {
            if (res == 0 || (errno != EAGAIN && errno != EINTR)) {
                snprintf(errstr, 1024, "Write error on socket (%d): %s", errno,
                         strerror(errno));
                sv_valid = 0;
                close(client_fd);
                return(-1);
            }
        } else {
            writebuf.erase(0, res);
        }
    }

    if (!FD_ISSET(client_fd, &read_set))
        return 0;

    char data[2048];
    memset(data, 0, 2048);

    //    while (1) {
    if (fgets(data, 2048, clientf) == NULL) {
        if (errno != 0)
            printf("errno %d\n", errno);
        if (errno != 0 && errno != EAGAIN) {
            snprintf(errstr, 1024, "Read error %d (%s)\n",
                     errno, sys_errlist[errno]);
            sv_valid = 0;
            close(client_fd);
            return (-1);
        }

        if (feof(clientf)) {
            snprintf(errstr, 1024, "socket returned EOF, server has closed the connection.");
            sv_valid = 0;
            close(client_fd);
            return (-2);
        }

        return (0);
    }

    //fprintf(stderr, "Got: '%s'\n", data);

    if (strlen(data) < 2)
        return 0;

    // Drop out now on a status event so we can get drawn
    int ret = ParseData(data);
    if (ret == 2)
        return 1;

    return ret;
}

void TcpClient::RemoveNetwork(string in_bssid) {
    if (net_map.find(in_bssid) != net_map.end())
        net_map.erase(net_map.find(in_bssid));
}

int TcpClient::ParseData(char *in_data) {
    char header[65];

    // fprintf(stderr, "About to parse: '%s'\n", in_data);

    if (sscanf(in_data, "%64[^:]", header) < 1) {
        //fprintf(stderr, "Failed to find header key...\n");
        return 0;
    }

    unsigned int hdrlen = strlen(header) + 2;
    if (hdrlen >= strlen(in_data))
        return 0;

    // fprintf(stderr, "%ld We think we got header: '%s'\n", this, header);

    if (!strncmp(header, "*TERMINATE", 64)) {
        sv_valid = 0;
        snprintf(errstr, 1024, "Server has terminated.\n");
        return -1;
    } else if (!strncmp(header, "*KISMET", 64)) {
        if (sscanf(in_data+hdrlen, "%d.%d.%d %d",
                   &major, &minor, &tiny, (int *) &start_time) < 1)
            return 0;
    } else if (!strncmp(header, "*TIME", 64)) {
        if (sscanf(in_data+hdrlen, "%d", (int *) &serv_time) < 1)
            return 0;

        /*
         printf("Time header %d\n", (int) serv_time);
         */
    } else if (!strncmp(header, "*NETWORK", 64)) {
        wireless_network net;

        int scanned;

        char bssid[18], ssid[256], beacon[256];
        short int range[4], mask[4], gate[4];

        float maxrate;

        scanned = sscanf(in_data+hdrlen, "%17s %d \001%255[^\001]\001 \001%255[^\001]\001 "
                         "%d %d %d %d %d %d %d %d %d %hd.%hd.%hd.%hd %hd.%hd.%hd.%hd %hd.%hd.%hd.%hd "
                         "%d %f %f %f %f %f %f %f %f %d %d %d %f %d %d %d %d %d %d %d %d %f %f %f "
                         "%lf %lf %lf %ld",
                         bssid, (int *) &net.type, ssid, beacon,
                         &net.llc_packets, &net.data_packets, &net.crypt_packets, &net.interesting_packets,
                         &net.channel, &net.wep, (int *) &net.first_time, (int *) &net.last_time,
                         (int *) &net.ipdata.atype, &range[0], &range[1], &range[2], &range[3],
                         &mask[0], &mask[1], &mask[2], &mask[3],
                         &gate[0], &gate[1], &gate[2], &gate[3],
                         &net.gps_fixed, &net.min_lat, &net.min_lon, &net.min_alt, &net.min_spd,
                         &net.max_lat, &net.max_lon, &net.max_alt, &net.max_spd,
                         /*
                         &net.gps_lat, &net.gps_lon, &net.gps_alt, &net.gps_spd, &net.gps_mode,
                         &net.first_lat, &net.first_lon, &net.first_alt, &net.first_spd,
                         &net.first_mode,
                         */
                         &net.ipdata.octets, &net.cloaked, &net.beacon,
                         &maxrate,
                         &net.manuf_id, &net.manuf_score,
                         &net.quality, &net.signal, &net.noise,
                         &net.best_quality, &net.best_signal, &net.best_noise,
                         &net.best_lat, &net.best_lon, &net.best_alt,
                         &net.aggregate_lat, &net.aggregate_lon, &net.aggregate_alt,
                         &net.aggregate_points);

        if (scanned < 53) {
            //fprintf(stderr, "Flubbed network, discarding...\n");
            return 0;
        }

        // Alignment issues on some platforms make this necessary
        unsigned int rawmac0, rawmac1, rawmac2, rawmac3, rawmac4, rawmac5;

        sscanf(bssid, "%02X:%02X:%02X:%02X:%02X:%02X",
               &rawmac0, &rawmac1, &rawmac2,
               &rawmac3, &rawmac4, &rawmac5);

        net.bssid_raw[0] = rawmac0;
        net.bssid_raw[1] = rawmac1;
        net.bssid_raw[2] = rawmac2;
        net.bssid_raw[3] = rawmac3;
        net.bssid_raw[4] = rawmac4;
        net.bssid_raw[5] = rawmac5;

        /*
        sscanf(bssid, "%02X:%02X:%02X:%02X:%02X:%02X",
               (unsigned int *) &net.bssid_raw[0], (unsigned int *) &net.bssid_raw[1],
               (unsigned int *) &net.bssid_raw[2], (unsigned int *) &net.bssid_raw[3],
               (unsigned int *) &net.bssid_raw[4], (unsigned int *) &net.bssid_raw[5]);
               */

        net.bssid = bssid;
        if (ssid[0] != '\002')
            net.ssid = ssid;
        if (beacon[0] != '\002')
            net.beacon_info = beacon;
        for (int x = 0; x < 4; x++) {
            net.ipdata.range_ip[x] = (uint8_t) range[x];
            net.ipdata.mask[x] = (uint8_t) mask[x];
            net.ipdata.gate_ip[x] = (uint8_t) gate[x];
        }

        net.maxrate = maxrate;

        net_map[net.bssid] = net;

        // fprintf(stderr, "dealing with net %s\n", ssid);
        // fprintf(stderr, "Set net %s\n", net_map[net.bssid].ssid.c_str());

    } else if (!strncmp(header, "*CLIENT", 64)) {
        short int ip[4];
        char bssid[18];
        char mac[18];
        int scanned;
        float maxrate;
        wireless_client *client = new wireless_client;

        scanned = sscanf(in_data+hdrlen, "%17s %17s %d %d %d %d %d %d %d %d %d "
                         "%f %f %f %f %f %f %f %f %lf %lf "
                         "%lf %ld %f %d %d %d %d %d %d %d "
                         "%f %f %f %d %hd.%hd.%hd.%hd",
                         bssid, mac, (int *) &client->type,
                         (int *) &client->first_time, (int *) &client->last_time,
                         (int *) &client->manuf_id, &client->manuf_score,
                         &client->data_packets, &client->crypt_packets,
                         &client->interesting_packets,
                         &client->gps_fixed, &client->min_lat, &client->min_lon,
                         &client->min_alt, &client->min_spd,
                         &client->max_lat, &client->max_lon, &client->max_alt,
                         &client->max_spd, &client->aggregate_lat, &client->aggregate_lon,
                         &client->aggregate_alt, &client->aggregate_points,
                         &maxrate, &client->metric,
                         &client->quality, &client->signal, &client->noise,
                         &client->best_quality, &client->best_signal, &client->best_noise,
                         &client->best_lat, &client->best_lon, &client->best_alt,
                         (int *) &client->ipdata.atype, &ip[0], &ip[1], &ip[2], &ip[3]);

        if (scanned < 38)
            return 0;

        client->mac = bssid;
        client->maxrate = maxrate;

        // Alignment issues on some platforms make this necessary
        unsigned int rawmac0, rawmac1, rawmac2, rawmac3, rawmac4, rawmac5;

        sscanf(mac, "%02X:%02X:%02X:%02X:%02X:%02X",
               &rawmac0, &rawmac1, &rawmac2,
               &rawmac3, &rawmac4, &rawmac5);

        client->raw_mac[0] = rawmac0;
        client->raw_mac[1] = rawmac1;
        client->raw_mac[2] = rawmac2;
        client->raw_mac[3] = rawmac3;
        client->raw_mac[4] = rawmac4;
        client->raw_mac[5] = rawmac5;

        for (unsigned int x = 0; x < 4; x++)
            client->ipdata.ip[x] = ip[x];

        if (net_map.find(bssid) != net_map.end())
            net_map[bssid].client_map[mac] = client;

    } else if (!strncmp(header, "*REMOVE", 64)) {

        // If we get a remove request flag it to die and the group code will
        // destroy it after ungrouping it

        char bssid[MAC_STR_LEN];
        if (sscanf(in_data+hdrlen, "%17s", bssid) < 1)
            return 0;

        if (net_map.find(bssid) != net_map.end()) {
            net_map[bssid].type = network_remove;
        }

    } else if (!strncmp(header, "*GPS", 64)) {
        if (sscanf(in_data+hdrlen, "%f %f %f %f %d", &lat, &lon, &alt, &spd, &mode) < 5)
            return 0;

    } else if (!strncmp(header, "*INFO", 64)) {
        char chan_details[1024];
        char chan_details_sec[1024];

        memset(chan_details, 0, 1024);
        memset(chan_details_sec, 0, 1024);

        unsigned int numchan;
        if (sscanf(in_data+hdrlen, "%d %d %d %d %d %d %d %d %d %d%1024s\n",
                   &num_networks, &num_packets,
                   &num_crypt, &num_interesting,
                   &num_noise, &num_dropped, &quality, &power, &noise, &numchan,
                   chan_details) < 11)
            return 0;

        for (unsigned int x = 0; x < CHANNEL_MAX && x < numchan; x++) {
            if (sscanf(chan_details, "%d %1024s\n",
                       &channel_graph[x].signal, chan_details_sec) < 1)
                break;
            strncpy(chan_details, chan_details_sec, 1024);
        }

    } else if (!strncmp(header, "*CISCO", 64)) {
        cdp_packet cdp;
        memset(&cdp, 0, sizeof(cdp_packet));
        char bssid[18];
        int cap0, cap1, cap2, cap3, cap4, cap5, cap6;
        short int cdpip[4];

        if (sscanf(in_data+hdrlen, "%17s \001%s\001 %hd.%hd.%hd.%hd \001%s\001 "
                   "%d:%d:%d:%d;%d;%d;%d \001%s\001 \001%s\001\n",
                   bssid, cdp.dev_id,
                   &cdpip[0], &cdpip[1], &cdpip[2], &cdpip[3],
                   cdp.interface, &cap0, &cap1, &cap2, &cap3, &cap4, &cap5, &cap6,
                   cdp.software, cdp.platform) < 16)
            return 0;

        cdp.ip[0] = cdpip[0];
        cdp.ip[1] = cdpip[1];
        cdp.ip[2] = cdpip[2];
        cdp.ip[3] = cdpip[3];

        if (net_map.find(bssid) == net_map.end())
            return 0;

        net_map[bssid].cisco_equip[cdp.dev_id] = cdp;

    } else if (!strncmp(header, "*STATUS", 64)) {
        if (sscanf(in_data+hdrlen, "%1023[^\n]\n", status) != 1)
            return 0;
        return 2;
    } else if (!strncmp(header, "*STRING", 64)) {
        char netstr[2048];
        if (sscanf(in_data+hdrlen, "%2047[^\n]\n", netstr) != 1)
            return 0;
        strings.push_back(netstr);
        if (strings.size() > maxstrings)
            strings.erase(strings.begin());
    } else if (!strncmp(header, "*PACKET", 64)) {
        packet_info packinfo;
        memset(&packinfo, 0, sizeof(packet_info));
        unsigned int smac[MAC_LEN], dmac[MAC_LEN], bmac[MAC_LEN];
        short int sip[4], dip[4];

        if (sscanf(in_data+hdrlen, "%d %d %d %d %d %02X:%02X:%02X:%02X:%02X:%02X "
                   "%02X:%02X:%02X:%02X:%02X:%02X %02X:%02X:%02X:%02X:%02X:%02X "
                   "\001%32[^\001]\001 %d %hd.%hd.%hd.%hd %hd.%hd.%hd.%hd %d %d %d "
                   "\001%16[^\001]\001\n",
                   (int *) &packinfo.type,
                   (int *) &packinfo.time,
                   &packinfo.encrypted, &packinfo.interesting, &packinfo.beacon,
                   &smac[0], &smac[1], &smac[2], &smac[3], &smac[4], &smac[5],
                   &dmac[0], &dmac[1], &dmac[2], &dmac[3], &dmac[4], &dmac[5],
                   &bmac[0], &bmac[1], &bmac[2], &bmac[3], &bmac[4], &bmac[5],
                   packinfo.ssid,
                   (int *) &packinfo.proto.type,
                   &sip[0], &sip[1], &sip[2], &sip[3], &dip[0], &dip[1], &dip[2], &dip[3],
                   (int *) &packinfo.proto.sport, (int *) &packinfo.proto.dport,
                   (int *) &packinfo.proto.nbtype, packinfo.proto.netbios_source) < 36)
            return 0;

        for (unsigned int x = 0; x < 6; x++) {
            packinfo.source_mac[x] = smac[x];
            packinfo.dest_mac[x] = dmac[x];
            packinfo.bssid_mac[x] = bmac[x];
        }

        for (unsigned int x = 0; x < 4; x++) {
            packinfo.proto.source_ip[x] = sip[x];
            packinfo.proto.dest_ip[x] = dip[x];
        }

        packinfos.push_back(packinfo);
        if (packinfos.size() > maxpackinfos)
            packinfos.erase(packinfos.begin());

    } else {
//        fprintf(stderr, "%ld we can't handle our header\n", this);
        return 0;
    }

    return 1;
}

time_t TcpClient::FetchServTime() {
    return serv_time;
}

int TcpClient::FetchLoc(float *in_lat, float *in_lon, float *in_alt, float *in_spd, int *in_mode) {
    *in_lat = lat; *in_lon = lon;
    *in_alt = alt; *in_spd = spd;
    *in_mode = mode;
    return mode;
}

vector<wireless_network *> TcpClient::FetchNetworkList() {
    vector<wireless_network *> retvec;

    for (map<string, wireless_network>::iterator x = net_map.begin(); x != net_map.end(); ++x) {
        retvec.push_back(&x->second);
    }

    return retvec;
}

vector<wireless_network *> TcpClient::FetchNthRecent(unsigned int n) {
    vector<wireless_network *> vec = FetchNetworkList();

    // XXX
    // This is much easier now that we use vectors.  We're already ordered
    // by time since we're inserted in order, so we can just erase...
    // XXX
    sort(vec.begin(), vec.end(), SortLastTimeLT());

    int drop = vec.size() - n;

    if (drop > 0) {
        vec.erase(vec.begin(), vec.begin() + drop);
    }

    sort(vec.begin(), vec.end(), SortFirstTimeLT());

    return vec;
}

int TcpClient::FetchChannelPower(int in_channel) {
    if (in_channel > 0 && in_channel < CHANNEL_MAX)
        return channel_graph[in_channel - 1].signal;

    return -1;
}

int TcpClient::Send(const char *in_data) {
    if (!sv_valid) {
        snprintf(errstr, 1024, "TcpClient poll() on an inactive connection.");
        return -1;
    }

    writebuf += in_data;

    return 1;
}
