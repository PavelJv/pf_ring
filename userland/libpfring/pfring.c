/*
 *
 * (C) 2005-07 - Luca Deri <deri@ntop.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "pfring.h"

// #define RING_DEBUG

/* ******************************* */

unsigned long long rdtsc() {
  unsigned long long a;
  asm volatile("rdtsc":"=A" (a));
  return(a);
}

/* ******************************* */

int pfring_set_cluster(pfring *ring, u_int clusterId) {
  return(ring ? setsockopt(ring->fd, 0, SO_ADD_TO_CLUSTER,
			      &clusterId, sizeof(clusterId)): -1);
}

/* ******************************* */

int pfring_remove_from_cluster(pfring *ring) {
  return(ring ? setsockopt(ring->fd, 0, SO_REMOVE_FROM_CLUSTER, 
			      NULL, 0) : -1);
}

/* ******************************* */

int pfring_set_reflector(pfring *ring, char *reflectorDevice) {
  return(ring ?
	 setsockopt(ring->fd, 0, SO_SET_REFLECTOR,
		    &reflectorDevice, strlen(reflectorDevice)) : -1);
}

/* ******************************* */

static int set_if_promisc(const char *device, int set_promisc) {
  int sock_fd;
  struct ifreq ifr;

  if(device == NULL) return(-3);

  sock_fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if(sock_fd <= 0) return(-1);
  
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, device, sizeof(ifr.ifr_name));
  if(ioctl(sock_fd, SIOCGIFFLAGS, &ifr) == -1) {  
    close(sock_fd);
    return(-2);
  }	  

  if(set_promisc) {
    if((ifr.ifr_flags & IFF_PROMISC) == 0) ifr.ifr_flags |= IFF_PROMISC;
  } else {
    /* Remove promisc */
    if((ifr.ifr_flags & IFF_PROMISC) != 0) ifr.ifr_flags &= ~IFF_PROMISC;    
  }
  
  if(ioctl(sock_fd, SIOCSIFFLAGS, &ifr) == -1) 
    return(-1);
  
  close(sock_fd);
  return(0);
}

/* **************************************************** */

pfring* pfring_open(char *device_name, int promisc) {
  int err = 0;
  pfring *ring = (pfring*)malloc(sizeof(pfring));
  
  if(ring == NULL) 
    return(NULL);
  else
    memset(ring, 0, sizeof(pfring));

  ring->fd = socket(PF_RING, SOCK_RAW, htons(ETH_P_ALL));

#ifdef RING_DEBUG
  printf("Open RING [fd=%d]\n", ring->fd);
#endif

  if(ring->fd > 0) {
    struct sockaddr sa;
    int             rc;
    u_int memSlotsLen;

    sa.sa_family   = PF_RING;
    snprintf(sa.sa_data, sizeof(sa.sa_data), "%s", device_name);
    rc = bind(ring->fd, (struct sockaddr *)&sa, sizeof(sa));

    if(rc == 0) {
      ring->buffer = (char *)mmap(NULL, PAGE_SIZE,
				       PROT_READ|PROT_WRITE,
				       MAP_SHARED,
				       ring->fd, 0);

      if(ring->buffer == MAP_FAILED) {
	printf("mmap() failed");
	free(ring);
	return (NULL);	
      }

      ring->slots_info = (FlowSlotInfo *)ring->buffer;
      if(ring->slots_info->version != RING_FLOWSLOT_VERSION) {
	printf("Wrong RING version: "
	       "kernel is %i, libpcap was compiled with %i\n",
	       ring->slots_info->version, RING_FLOWSLOT_VERSION);
	free(ring); 
	return (NULL);
      }
      memSlotsLen = ring->slots_info->tot_mem;
      munmap(ring->buffer, PAGE_SIZE);

      ring->buffer = (char *)mmap(NULL, memSlotsLen,
				 PROT_READ|PROT_WRITE,
				 MAP_SHARED, ring->fd, 0);

      if(ring->buffer == MAP_FAILED) {
	printf("mmap() failed");
	free(ring);
	return (NULL);
      }

      ring->slots_info   = (FlowSlotInfo *)ring->buffer;
      ring->slots = (char *)(ring->buffer+sizeof(FlowSlotInfo));

      /* Safety check */
      if(ring->slots_info->remove_idx >= ring->slots_info->tot_slots)
	ring->slots_info->remove_idx = 0;

      ring->page_id = PAGE_SIZE, ring->slot_id = 0, ring->pkts_per_page = 0;

      /* Set defaults */
      ring->device_name = strdup(device_name);

#ifdef RING_DEBUG
      printf("RING (%s): tot_slots=%d/slot_len=%d/"
	     "insertIdx=%d/remove_idx=%d/dropped=%d\n",
	     device_name,
	     ring->slots_info->tot_slots,
	     ring->slots_info->slot_len,
	     ring->slots_info->insert_idx,
	     ring->slots_info->remove_idx,
	     ring->slots_info->tot_lost);
#endif

      if(promisc) {
	if(set_if_promisc(device_name, 1) == 0)
	  ring->clear_promisc = 1;
      }

    } else {
      close(ring->fd);
      err = -1;
    }
  } else {
    err = -1; 
    free(ring);
  }

  return(err == 0 ? ring : NULL);
}

/* **************************************************** */

void pfring_close(pfring *ring) {
  if(!ring) return;

  if(ring->buffer != NULL) {
    munmap(ring->buffer, ring->slots_info->tot_mem);
  }

  if(ring->clear_promisc)
    set_if_promisc(ring->device_name, 0);

  free(ring->device_name);
  close(ring->fd);
  free(ring);
}

/* **************************************************** */

int pfring_toggle_filtering_policy(pfring *ring, u_int8_t rules_default_accept_policy) {
  return(ring ? setsockopt(ring->fd, 0, SO_TOGGLE_FILTER_POLICY, 
			   &rules_default_accept_policy,
			   sizeof(rules_default_accept_policy)): -1);

}

/* **************************************************** */

int pfring_version(pfring *ring, u_int32_t *version) {
  if(ring == NULL) 
    return(-1);
  else {
    socklen_t len = sizeof(u_int32_t);
    return(getsockopt(ring->fd, 0, SO_GET_RING_VERSION, version, &len));
  }
}

/* **************************************************** */

int pfring_get_filtering_rule_stats(pfring *ring, u_int16_t rule_id,
				    char* stats, u_int *stats_len) {
  if((ring == NULL) || (*stats_len < sizeof(u_int16_t)))
    return(-1);
  else {
    memcpy(stats, &rule_id, sizeof(u_int16_t));
    return(getsockopt(ring->fd, 0,
		      SO_GET_FILTERING_RULE_STATS,
		      stats, stats_len));
  }
}

/* **************************************************** */

int pfring_set_filtering_rule_plugin_id(pfring *ring, 
					u_int16_t rule_id,
					u_int16_t plugin_id) {
  if(ring == NULL)
    return(-1);
  else {
    struct rule_plugin_id info;
    int rc;

    info.plugin_id = plugin_id;
    info.rule_id   = rule_id;

    rc = setsockopt(ring->fd, 0, 
		    SO_SET_FILTERING_RULE_PLUGIN_ID,
		    &info, sizeof(info));
    return(rc);
  }
}

/* **************************************************** */

int pfring_add_filtering_rule(pfring *ring, filtering_rule* rule_to_add) {
  int rc;

  if((!rule_to_add) || (!ring)) return(-1);

  /* Sanitize entry */
  if(rule_to_add->port_low > rule_to_add->port_high)
    rule_to_add->port_low = rule_to_add->port_high;
  
  if(rule_to_add->balance_id > rule_to_add->balance_pool)
    rule_to_add->balance_id = 0;
  
  if(rule_to_add->host_netmask > 0)
    rule_to_add->host_ip = (rule_to_add->host_ip & rule_to_add->host_netmask);

  rc = setsockopt(ring->fd, 0, SO_ADD_FILTERING_RULE,
		  rule_to_add, sizeof(filtering_rule));

  return(rc);
}

/* **************************************************** */

int pfring_remove_filtering_rule(pfring *ring, u_int16_t rule_id) {
  int rc;

  rc = ring ? setsockopt(ring->fd, 0, SO_REMOVE_FILTERING_RULE,
			 &rule_id, sizeof(rule_id)): -1;

  return(rc);
}

/* **************************************************** */

int pfring_set_sampling_rate(pfring *ring, u_int32_t rate /* 1 = no sampling */) {
  int rc;

  rc = ring ? setsockopt(ring->fd, 0, SO_SET_SAMPLING_RATE,
			 &rate, sizeof(rate)): -1;

  return(rc);
}

/* ******************************* */

int pfring_stats(pfring *ring, pfring_stat *stats) {
  if(ring && stats) {
    stats->recv = ring->slots_info->tot_read;
    stats->drop = ring->slots_info->tot_lost;
    return(1);
  } else
    return(0);
}

/* **************************************************** */

int pfring_recv(pfring *ring, char* buffer, int buffer_len, 
		struct pfring_pkthdr *hdr, u_char wait_for_incoming_packet) {
  u_int idx, numRuns = 0, ptrAddr;
  FlowSlot *slot;
  u_int32_t queuedPkts;

  if((ring == NULL) || (ring->buffer == NULL)) return(-1);
	  
  slot = (FlowSlot*)&ring->slots[ring->slots_info->remove_idx*ring->slots_info->slot_len];

 do_pfring_recv:
  if(ring->slots_info->tot_insert >= ring->slots_info->tot_read)
    queuedPkts = ring->slots_info->tot_insert - ring->slots_info->tot_read;
  else
    queuedPkts = ring->slots_info->tot_slots + ring->slots_info->tot_insert - ring->slots_info->tot_read;

  if(queuedPkts && (slot->slot_state == 1 /* There's a packet to read */)) {
    char *bucket = (char*)&slot->bucket;
    struct pfring_pkthdr *_hdr = (struct pfring_pkthdr*)bucket;
    int bktLen = _hdr->caplen;

    if(bktLen > buffer_len) bktLen = buffer_len;
      
    if(buffer) memcpy(buffer, &bucket[sizeof(struct pfring_pkthdr)], bktLen);

    if(ring->slots_info->remove_idx >= (ring->slots_info->tot_slots-1)) {
      ring->slots_info->remove_idx = 0;
      ring->page_id = PAGE_SIZE, ring->slot_id = 0, ring->pkts_per_page = 0;
    } else {
      ring->slots_info->remove_idx++;
      ring->pkts_per_page++, ring->slot_id += ring->slots_info->slot_len;
    }
      
    if(hdr) memcpy(hdr, _hdr, sizeof(struct pfring_pkthdr));

    ring->slots_info->tot_read++;
    slot->slot_state = 0; /* Empty slot */
      
    return(1);
  } else if(wait_for_incoming_packet) {
    struct pollfd pfd;
    int rc;

    /* Sleep when nothing is happening */
    pfd.fd      = ring->fd;
    pfd.events  = POLLIN|POLLERR;
    pfd.revents = 0;

#ifdef RING_DEBUG
    printf("==>> poll [remove_idx=%u][insert_idx=%u][loss=%d][queuedPkts=%u]"
	   "[slot_state=%d][tot_insert=%u][tot_read=%u]\n",
	   ring->slots_info->remove_idx,
	   ring->slots_info->insert_idx,
	   ring->slots_info->tot_lost, 
	   queuedPkts, slot->slot_state,
	   ring->slots_info->tot_insert,
	   ring->slots_info->tot_read);
#endif
	      
    errno = 0;

    rc = poll(&pfd, 1, -1);

    ring->num_poll_calls++;

    if(rc == -1)
      return(-1);
    else
      goto do_pfring_recv;
  }

  return(-1); /* Not reached */
}
