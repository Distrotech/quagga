
/* 
 * Interface functions.
 * Copyright (C) 1997, 98 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 * 
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <zebra.h>

#include "linklist.h"
#include "vector.h"
#include "vty.h"
#include "command.h"
#include "if.h"
#include "sockunion.h"
#include "prefix.h"
#include "memory.h"
#include "table.h"
#include "buffer.h"
#include "str.h"
#include "log.h"

/* Master list of interfaces. */
struct list *iflist;

/* One for each program.  This structure is needed to store hooks. */
struct if_master
{
  int (*if_new_hook) (struct interface *);
  int (*if_delete_hook) (struct interface *);
} if_master;

/* Compare interface names, returning an integer greater than, equal to, or
 * less than 0, (following the strcmp convention), according to the
 * relationship between ifp1 and ifp2.  Interface names consist of an
 * alphabetic prefix and a numeric suffix.  The primary sort key is
 * lexicographic by name, and then numeric by number.  No number sorts
 * before all numbers.  Examples: de0 < de1, de100 < fxp0 < xl0, devpty <
 * devpty0, de0 < del0
 */         
int
if_cmp_func (struct interface *ifp1, struct interface *ifp2)
{
  unsigned int l1, l2;
  long int x1, x2;
  char *p1, *p2;
  int res;

  p1 = ifp1->name;
  p2 = ifp2->name;

  while (*p1 && *p2) {
    /* look up to any number */
    l1 = strcspn(p1, "0123456789");
    l2 = strcspn(p2, "0123456789");

    /* name lengths are different -> compare names */
    if (l1 != l2)
      return (strcmp(p1, p2));

    /* Note that this relies on all numbers being less than all letters, so
     * that de0 < del0.
     */
    res = strncmp(p1, p2, l1);

    /* names are different -> compare them */
    if (res)
      return res;

    /* with identical name part, go to numeric part */
    p1 += l1;
    p2 += l1;

    if (!*p1) 
      return -1;
    if (!*p2) 
      return 1;

    x1 = strtol(p1, &p1, 10);
    x2 = strtol(p2, &p2, 10);

    /* let's compare numbers now */
    if (x1 < x2)
      return -1;
    if (x1 > x2)
      return 1;

    /* numbers were equal, lets do it again..
    (it happens with name like "eth123.456:789") */
  }
  if (*p1)
    return 1;
  if (*p2)
    return -1;
  return 0;
}

/* Create new interface structure. */
struct interface *
if_new ()
{
  struct interface *ifp;

  ifp = XMALLOC (MTYPE_IF, sizeof (struct interface));
  memset (ifp, 0, sizeof (struct interface));
  return ifp;
}

struct interface *
if_create (const char *name, int namelen)
{
  struct interface *ifp;

  ifp = if_new ();
  
  assert (name);
  assert (namelen <= (INTERFACE_NAMSIZ + 1));
  strncpy (ifp->name, name, namelen);
  ifp->name[INTERFACE_NAMSIZ] = '\0';
  if (if_lookup_by_name(ifp->name) == NULL)
    listnode_add_sort (iflist, ifp);
  ifp->connected = list_new ();
  ifp->connected->del = (void (*) (void *)) connected_free;

  if (if_master.if_new_hook)
    (*if_master.if_new_hook) (ifp);

  return ifp;
}

/* Delete and free interface structure. */
void
if_delete (struct interface *ifp)
{
  listnode_delete (iflist, ifp);

  if (if_master.if_delete_hook)
    (*if_master.if_delete_hook) (ifp);

  /* Free connected address list */
  list_delete (ifp->connected);

  XFREE (MTYPE_IF, ifp);
}

/* Add hook to interface master. */
void
if_add_hook (int type, int (*func)(struct interface *ifp))
{
  switch (type) {
  case IF_NEW_HOOK:
    if_master.if_new_hook = func;
    break;
  case IF_DELETE_HOOK:
    if_master.if_delete_hook = func;
    break;
  default:
    break;
  }
}

/* Interface existance check by index. */
struct interface *
if_lookup_by_index (unsigned int index)
{
  struct listnode *node;
  struct interface *ifp;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);
      if (ifp->ifindex == index)
	return ifp;
    }
  return NULL;
}

char *
ifindex2ifname (unsigned int index)
{
  struct listnode *node;
  struct interface *ifp;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);
      if (ifp->ifindex == index)
	return ifp->name;
    }
  return (char *) "unknown";
}

/* Interface existance check by interface name. */
struct interface *
if_lookup_by_name (const char *name)
{
  struct listnode *node;
  struct interface *ifp;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);
      if (strncmp (name, ifp->name, sizeof ifp->name) == 0)
	return ifp;
    }
  return NULL;
}

/* Lookup interface by IPv4 address. */
struct interface *
if_lookup_exact_address (struct in_addr src)
{
  struct listnode *node;
  struct listnode *cnode;
  struct interface *ifp;
  struct prefix *p;
  struct connected *c;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);

      for (cnode = listhead (ifp->connected); cnode; nextnode (cnode))
	{
	  c = getdata (cnode);

	  p = c->address;

	  if (p && p->family == AF_INET)
	    {
	      if (IPV4_ADDR_SAME (&p->u.prefix4, &src))
		return ifp;
	    }	      
	}
    }
  return NULL;
}

/* Lookup interface by IPv4 address. */
struct interface *
if_lookup_address (struct in_addr src)
{
  struct listnode *node;
  struct prefix addr;
  int bestlen = 0;
  struct listnode *cnode;
  struct interface *ifp;
  struct prefix *p;
  struct connected *c;
  struct interface *match;

  addr.family = AF_INET;
  addr.u.prefix4 = src;
  addr.prefixlen = IPV4_MAX_BITLEN;

  match = NULL;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);

      for (cnode = listhead (ifp->connected); cnode; nextnode (cnode))
	{
	  c = getdata (cnode);

	  if (c->address && (c->address->family == AF_INET))
	    {
	      if (CONNECTED_POINTOPOINT_HOST(c))
		{
		 /* PTP  links are conventionally identified 
		    by the address of the far end - MAG */
		  if (IPV4_ADDR_SAME (&c->destination->u.prefix4, &src))
		    return ifp;
		}
	      else
		{
		  p = c->address;

		  if (prefix_match (p, &addr) && p->prefixlen > bestlen)
		    {
		      bestlen = p->prefixlen;
		      match = ifp;
		    }
		}
	    }
	}
    }
  return match;
}

/* Get interface by name if given name interface doesn't exist create
   one. */
struct interface *
if_get_by_name (const char *name)
{
  struct interface *ifp;

  ifp = if_lookup_by_name (name);
  if (ifp == NULL)
    ifp = if_create (name, INTERFACE_NAMSIZ);
  return ifp;
}

/* Does interface up ? */
int
if_is_up (struct interface *ifp)
{
  return ifp->flags & IFF_UP;
}

/* Is interface running? */
int
if_is_running (struct interface *ifp)
{
  return ifp->flags & IFF_RUNNING;
}

/* Is the interface operative, eg. either UP & RUNNING
   or UP & !ZEBRA_INTERFACE_LINK_DETECTION */
int
if_is_operative (struct interface *ifp)
{
  return ((ifp->flags & IFF_UP) &&
	  (ifp->flags & IFF_RUNNING || !CHECK_FLAG(ifp->status, ZEBRA_INTERFACE_LINKDETECTION)));
}

/* Is this loopback interface ? */
int
if_is_loopback (struct interface *ifp)
{
  return ifp->flags & IFF_LOOPBACK;
}

/* Does this interface support broadcast ? */
int
if_is_broadcast (struct interface *ifp)
{
  return ifp->flags & IFF_BROADCAST;
}

/* Does this interface support broadcast ? */
int
if_is_pointopoint (struct interface *ifp)
{
  return ifp->flags & IFF_POINTOPOINT;
}

/* Does this interface support multicast ? */
int
if_is_multicast (struct interface *ifp)
{
  return ifp->flags & IFF_MULTICAST;
}

/* Printout flag information into log */
const char *
if_flag_dump (unsigned long flag)
{
  int separator = 0;
  static char logbuf[BUFSIZ];

#define IFF_OUT_LOG(X,STR) \
  if ((X) && (flag & (X))) \
    { \
      if (separator) \
	strlcat (logbuf, ",", BUFSIZ); \
      else \
	separator = 1; \
      strlcat (logbuf, STR, BUFSIZ); \
    }

  strlcpy (logbuf, "  <", BUFSIZ);
  IFF_OUT_LOG (IFF_UP, "UP");
  IFF_OUT_LOG (IFF_BROADCAST, "BROADCAST");
  IFF_OUT_LOG (IFF_DEBUG, "DEBUG");
  IFF_OUT_LOG (IFF_LOOPBACK, "LOOPBACK");
  IFF_OUT_LOG (IFF_POINTOPOINT, "POINTOPOINT");
  IFF_OUT_LOG (IFF_NOTRAILERS, "NOTRAILERS");
  IFF_OUT_LOG (IFF_RUNNING, "RUNNING");
  IFF_OUT_LOG (IFF_NOARP, "NOARP");
  IFF_OUT_LOG (IFF_PROMISC, "PROMISC");
  IFF_OUT_LOG (IFF_ALLMULTI, "ALLMULTI");
  IFF_OUT_LOG (IFF_OACTIVE, "OACTIVE");
  IFF_OUT_LOG (IFF_SIMPLEX, "SIMPLEX");
  IFF_OUT_LOG (IFF_LINK0, "LINK0");
  IFF_OUT_LOG (IFF_LINK1, "LINK1");
  IFF_OUT_LOG (IFF_LINK2, "LINK2");
  IFF_OUT_LOG (IFF_MULTICAST, "MULTICAST");
#ifdef SOLARIS_IPV6
  IFF_OUT_LOG (IFF_IPV4, "IFF_IPv4");
  IFF_OUT_LOG (IFF_IPV6, "IFF_IPv6");
#endif /* SOLARIS_IPV6 */

  strlcat (logbuf, ">", BUFSIZ);

  return logbuf;
}

/* For debugging */
void
if_dump (struct interface *ifp)
{
  struct listnode *node;

  zlog_info ("Interface %s index %d metric %d mtu %d "
#ifdef HAVE_IPV6
             "mtu6 %d "
#endif /* HAVE_IPV6 */
             "%s",
	     ifp->name, ifp->ifindex, ifp->metric, ifp->mtu, 
#ifdef HAVE_IPV6
	     ifp->mtu6,
#endif /* HAVE_IPV6 */
	     if_flag_dump (ifp->flags));
  
  for (node = listhead (ifp->connected); node; nextnode (node))
    ;
}

/* Interface printing for all interface. */
void
if_dump_all ()
{
  struct listnode *node;

  for (node = listhead (iflist); node; nextnode (node))
    if_dump (getdata (node));
}

DEFUN (interface_desc, 
       interface_desc_cmd,
       "description .LINE",
       "Interface specific description\n"
       "Characters describing this interface\n")
{
  int i;
  struct interface *ifp;
  struct buffer *b;

  if (argc == 0)
    return CMD_SUCCESS;

  ifp = vty->index;
  if (ifp->desc)
    XFREE (0, ifp->desc);

  b = buffer_new (1024);
  for (i = 0; i < argc; i++)
    {
      buffer_putstr (b, argv[i]);
      buffer_putc (b, ' ');
    }
  buffer_putc (b, '\0');

  ifp->desc = buffer_getstr (b);
  buffer_free (b);

  return CMD_SUCCESS;
}

DEFUN (no_interface_desc, 
       no_interface_desc_cmd,
       "no description",
       NO_STR
       "Interface specific description\n")
{
  struct interface *ifp;

  ifp = vty->index;
  if (ifp->desc)
    XFREE (0, ifp->desc);
  ifp->desc = NULL;

  return CMD_SUCCESS;
}


/* See also wrapper function zebra_interface() in zebra/interface.c */
DEFUN (interface,
       interface_cmd,
       "interface IFNAME",
       "Select an interface to configure\n"
       "Interface's name\n")
{
  struct interface *ifp;

  ifp = if_lookup_by_name (argv[0]);

  if (ifp == NULL)
    ifp = if_create (argv[0], INTERFACE_NAMSIZ);
  vty->index = ifp;
  vty->node = INTERFACE_NODE;

  return CMD_SUCCESS;
}

DEFUN_NOSH (no_interface,
           no_interface_cmd,
           "no interface IFNAME",
           NO_STR
           "Delete a pseudo interface's configuration\n"
           "Interface's name\n")
{
  // deleting interface
  struct interface *ifp;

  ifp = if_lookup_by_name (argv[0]);

  if (ifp == NULL)
  {
    vty_out (vty, "%% Inteface %s does not exist%s", argv[0], VTY_NEWLINE);
    return CMD_WARNING;
  }

  if (CHECK_FLAG (ifp->status, ZEBRA_INTERFACE_ACTIVE)) 
  {
    vty_out (vty, "%% Only inactive interfaces can be deleted%s",
            VTY_NEWLINE);
    return CMD_WARNING;
  }

  if_delete(ifp);

  return CMD_SUCCESS;
}

/* For debug purpose. */
DEFUN (show_address,
       show_address_cmd,
       "show address",
       SHOW_STR
       "address\n")
{
  struct listnode *node;
  struct listnode *node2;
  struct interface *ifp;
  struct connected *ifc;
  struct prefix *p;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);

      for (node2 = listhead (ifp->connected); node2; nextnode (node2))
	{
	  ifc = getdata (node2);
	  p = ifc->address;

	  if (p->family == AF_INET)
	    vty_out (vty, "%s/%d%s", inet_ntoa (p->u.prefix4), p->prefixlen,
		     VTY_NEWLINE);
	}
    }
  return CMD_SUCCESS;
}

/* Allocate connected structure. */
struct connected *
connected_new ()
{
  struct connected *new = XMALLOC (MTYPE_CONNECTED, sizeof (struct connected));
  memset (new, 0, sizeof (struct connected));
  return new;
}

/* Free connected structure. */
void
connected_free (struct connected *connected)
{
  if (connected->address)
    prefix_free (connected->address);

  if (connected->destination)
    prefix_free (connected->destination);

  if (connected->label)
    free (connected->label);

  XFREE (MTYPE_CONNECTED, connected);
}

/* Print if_addr structure. */
void
connected_log (struct connected *connected, char *str)
{
  struct prefix *p;
  struct interface *ifp;
  char logbuf[BUFSIZ];
  char buf[BUFSIZ];
  
  ifp = connected->ifp;
  p = connected->address;

  snprintf (logbuf, BUFSIZ, "%s interface %s %s %s/%d ", 
	    str, ifp->name, prefix_family_str (p),
	    inet_ntop (p->family, &p->u.prefix, buf, BUFSIZ),
	    p->prefixlen);

  p = connected->destination;
  if (p)
    {
      strncat (logbuf, inet_ntop (p->family, &p->u.prefix, buf, BUFSIZ),
	       BUFSIZ - strlen(logbuf));
    }
  zlog (NULL, LOG_INFO, logbuf);
}

/* If two connected address has same prefix return 1. */
int
connected_same_prefix (struct prefix *p1, struct prefix *p2)
{
  if (p1->family == p2->family)
    {
      if (p1->family == AF_INET &&
	  IPV4_ADDR_SAME (&p1->u.prefix4, &p2->u.prefix4))
	return 1;
#ifdef HAVE_IPV6
      if (p1->family == AF_INET6 &&
	  IPV6_ADDR_SAME (&p1->u.prefix6, &p2->u.prefix6))
	return 1;
#endif /* HAVE_IPV6 */
    }
  return 0;
}

struct connected *
connected_delete_by_prefix (struct interface *ifp, struct prefix *p)
{
  struct listnode *node;
  struct listnode *next;
  struct connected *ifc;

  /* In case of same prefix come, replace it with new one. */
  for (node = listhead (ifp->connected); node; node = next)
    {
      ifc = getdata (node);
      next = node->next;

      if (connected_same_prefix (ifc->address, p))
	{
	  listnode_delete (ifp->connected, ifc);
	  return ifc;
	}
    }
  return NULL;
}

/* Find the IPv4 address on our side that will be used when packets
   are sent to dst. */
struct connected *
connected_lookup_address (struct interface *ifp, struct in_addr dst)
{
  struct prefix addr;
  struct listnode *cnode;
  struct prefix *p;
  struct connected *c;
  struct connected *match;

  addr.family = AF_INET;
  addr.u.prefix4 = dst;
  addr.prefixlen = IPV4_MAX_BITLEN;

  match = NULL;

  for (cnode = listhead (ifp->connected); cnode; nextnode (cnode))
    {
      c = getdata (cnode);

      if (c->address && (c->address->family == AF_INET))
        {
	  if (CONNECTED_POINTOPOINT_HOST(c))
	    {
		     /* PTP  links are conventionally identified 
			by the address of the far end - MAG */
	      if (IPV4_ADDR_SAME (&c->destination->u.prefix4, &dst))
		return c;
	    }
	  else
	    {
	      p = c->address;

	      if (prefix_match (p, &addr) &&
	      	  (!match || (p->prefixlen > match->address->prefixlen)))
		match = c;
	    }
        }
    }
  return match;
}

struct connected *
connected_add_by_prefix (struct interface *ifp, struct prefix *p, 
                         struct prefix *destination)
{
  struct connected *ifc;

  /* Allocate new connected address. */
  ifc = connected_new ();
  ifc->ifp = ifp;

  /* Fetch interface address */
  ifc->address = prefix_new();
  memcpy (ifc->address, p, sizeof(struct prefix));

  /* Fetch dest address */
  if (destination)
    {
      ifc->destination = prefix_new();
      memcpy (ifc->destination, destination, sizeof(struct prefix));
    }

  /* Add connected address to the interface. */
  listnode_add (ifp->connected, ifc);
  return ifc;
}

#ifndef HAVE_IF_NAMETOINDEX
unsigned int
if_nametoindex (const char *name)
{
  struct listnode *node;
  struct interface *ifp;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);
      if (strcmp (ifp->name, name) == 0)
	return ifp->ifindex;
    }
  return 0;
}
#endif

#ifndef HAVE_IF_INDEXTONAME
char *
if_indextoname (unsigned int ifindex, char *name)
{
  struct listnode *node;
  struct interface *ifp;

  for (node = listhead (iflist); node; nextnode (node))
    {
      ifp = getdata (node);
      if (ifp->ifindex == ifindex)
	{
	  memcpy (name, ifp->name, IFNAMSIZ);
	  return ifp->name;
	}
    }
  return NULL;
}
#endif

/* Interface looking up by interface's address. */

/* Interface's IPv4 address reverse lookup table. */
struct route_table *ifaddr_ipv4_table;
/* struct route_table *ifaddr_ipv6_table; */

void
ifaddr_ipv4_add (struct in_addr *ifaddr, struct interface *ifp)
{
  struct route_node *rn;
  struct prefix_ipv4 p;

  p.family = AF_INET;
  p.prefixlen = IPV4_MAX_PREFIXLEN;
  p.prefix = *ifaddr;

  rn = route_node_get (ifaddr_ipv4_table, (struct prefix *) &p);
  if (rn)
    {
      route_unlock_node (rn);
      zlog_info ("ifaddr_ipv4_add(): address %s is already added",
		 inet_ntoa (*ifaddr));
      return;
    }
  rn->info = ifp;
}

void
ifaddr_ipv4_delete (struct in_addr *ifaddr, struct interface *ifp)
{
  struct route_node *rn;
  struct prefix_ipv4 p;

  p.family = AF_INET;
  p.prefixlen = IPV4_MAX_PREFIXLEN;
  p.prefix = *ifaddr;

  rn = route_node_lookup (ifaddr_ipv4_table, (struct prefix *) &p);
  if (! rn)
    {
      zlog_info ("ifaddr_ipv4_delete(): can't find address %s",
		 inet_ntoa (*ifaddr));
      return;
    }
  rn->info = NULL;
  route_unlock_node (rn);
  route_unlock_node (rn);
}

/* Lookup interface by interface's IP address or interface index. */
struct interface *
ifaddr_ipv4_lookup (struct in_addr *addr, unsigned int ifindex)
{
  struct prefix_ipv4 p;
  struct route_node *rn;
  struct interface *ifp;
  struct listnode *node;

  if (addr)
    {
      p.family = AF_INET;
      p.prefixlen = IPV4_MAX_PREFIXLEN;
      p.prefix = *addr;

      rn = route_node_lookup (ifaddr_ipv4_table, (struct prefix *) &p);
      if (! rn)
	return NULL;
      
      ifp = rn->info;
      route_unlock_node (rn);
      return ifp;
    }
  else
    {
      for (node = listhead (iflist); node; nextnode (node))
	{
	  ifp = getdata (node);

	  if (ifp->ifindex == ifindex)
	    return ifp;
	}
    }
  return NULL;
}

/* Initialize interface list. */
void
if_init ()
{
  iflist = list_new ();
  ifaddr_ipv4_table = route_table_init ();

  if (iflist) {
    iflist->cmp = (int (*)(void *, void *))if_cmp_func;
    return;
  }

  memset (&if_master, 0, sizeof if_master);
}
