/*****************************************************************************
 * udp.c:
 *****************************************************************************
 * Copyright (C) 2004-2006 the VideoLAN team
 * Copyright © 2006 Rémi Denis-Courmont
 *
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@videolan.org>
 *          Rémi Denis-Courmont <rem # videolan.org>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>
#include <vlc/vlc.h>

#include <errno.h>
#include <assert.h>

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include <vlc_network.h>

#ifdef WIN32
#   if defined(UNDER_CE)
#       undef IP_MULTICAST_TTL
#       define IP_MULTICAST_TTL 3
#       undef IP_ADD_MEMBERSHIP
#       define IP_ADD_MEMBERSHIP 5
#   endif
#   define EAFNOSUPPORT WSAEAFNOSUPPORT
#   define if_nametoindex( str ) atoi( str )
#else
#   include <unistd.h>
#   ifdef HAVE_NET_IF_H
#       include <net/if.h>
#   endif
#endif

#ifndef SOL_IP
# define SOL_IP IPPROTO_IP
#endif
#ifndef SOL_IPV6
# define SOL_IPV6 IPPROTO_IPV6
#endif
#ifndef IPPROTO_IPV6
# define IPPROTO_IPV6 41
#endif

extern int net_Socket( vlc_object_t *p_this, int i_family, int i_socktype,
                       int i_protocol );


/*
 * XXX: I am too lazy to put all these dual functions in “next generation”
 * network plugins.
 */

static int net_SetMcastHopLimit( vlc_object_t *p_this,
                                 int fd, int family, int hlim )
{
    int proto, cmd;

    /* There is some confusion in the world whether IP_MULTICAST_TTL 
     * takes a byte or an int as an argument.
     * BSD seems to indicate byte so we are going with that and use
     * int as a fallback to be safe */
    switch( family )
    {
#ifdef IP_MULTICAST_TTL
        case AF_INET:
            proto = SOL_IP;
            cmd = IP_MULTICAST_TTL;
            break;
#endif

#ifdef IPV6_MULTICAST_HOPS
        case AF_INET6:
            proto = SOL_IPV6;
            cmd = IPV6_MULTICAST_HOPS;
            break;
#endif

        default:
            msg_Warn( p_this, "%s", strerror( EAFNOSUPPORT ) );
            return VLC_EGENERIC;
    }

    if( setsockopt( fd, proto, cmd, &hlim, sizeof( hlim ) ) < 0 )
    {
        /* BSD compatibility */
        unsigned char buf;

        buf = (unsigned char)(( hlim > 255 ) ? 255 : hlim);
        if( setsockopt( fd, proto, cmd, &buf, sizeof( buf ) ) )
            return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}


static int net_SetMcastOutIface (int fd, int family, int scope)
{
    switch (family)
    {
#ifdef IPV6_MULTICAST_IF
        case AF_INET6:
            return setsockopt (fd, SOL_IPV6, IPV6_MULTICAST_IF,
                               &scope, sizeof (scope));
#endif

#ifdef __linux__
        case AF_INET:
        {
            struct ip_mreqn req = { .imr_ifindex = scope };

            return setsockopt (fd, SOL_IP, IP_MULTICAST_IF, &req,
                               sizeof (req));
        }
#endif
    }

    errno = EAFNOSUPPORT;
    return -1;
}


static inline int net_SetMcastOutIPv4 (int fd, struct in_addr ipv4)
{
#ifdef IP_MULTICAST_IF
    return setsockopt( fd, SOL_IP, IP_MULTICAST_IF, &ipv4, sizeof (ipv4));
#else
    errno = EAFNOSUPPORT;
    return -1;
#endif
}


static int net_SetMcastOut (vlc_object_t *p_this, int fd, int family,
                            const char *iface, const char *addr)
{
    if (iface != NULL)
    {
        int scope = if_nametoindex (iface);
        if (scope == 0)
        {
            msg_Err (p_this, "invalid multicast interface: %s", iface);
            return -1;
        }

        if (net_SetMcastOutIface (fd, family, scope) == 0)
            return 0;

        msg_Err (p_this, "%s: %s", iface, net_strerror (net_errno));
    }

    if (addr != NULL)
    {
        if (family == AF_INET)
        {
            struct in_addr ipv4;
            if (inet_pton (AF_INET, addr, &ipv4) <= 0)
            {
                msg_Err (p_this, "invalid IPv4 address for multicast: %s",
                         addr);
                return -1;
            }

            if (net_SetMcastOutIPv4 (fd, ipv4) == 0)
                return 0;

            msg_Err (p_this, "%s: %s", addr, net_strerror (net_errno));
        }
    }

    return -1;
}


/**
 * Old-style any-source multicast join.
 * In use on Windows XP/2003 and older.
 */
static int
net_IPv4Join (vlc_object_t *obj, int fd,
              const struct sockaddr_in *src, const struct sockaddr_in *grp)
{
#ifdef IP_ADD_MEMBERSHIP
    union
    {
        struct ip_mreq gr4;
# ifdef IP_ADD_SOURCE_MEMBERSHIP
        struct ip_mreq_source gsr4;
# endif
    } opt;
    int cmd;
    struct in_addr id = { .s_addr = INADDR_ANY };
    socklen_t optlen;

    /* Multicast interface IPv4 address */
    char *iface = var_CreateGetString (obj, "miface-addr");
    if (iface != NULL)
    {
        if ((*iface)
         && (inet_pton (AF_INET, iface, &id) <= 0))
        {
            msg_Err (obj, "invalid multicast interface address %s", iface);
            free (iface);
            goto error;
        }
    }

    memset (&opt, 0, sizeof (opt));
    if (src != NULL)
    {
# ifdef IP_ADD_SOURCE_MEMBERSHIP
        cmd = IP_ADD_SOURCE_MEMBERSHIP;
        opt.gsr4.imr_multiaddr = grp->sin_addr;
        opt.gsr4.imr_sourceaddr = src->sin_addr;
        opt.gsr4.imr_interface = id;
        optlen = sizeof (opt.gsr4);
# else
        errno = ENOSYS;
        goto error;
# endif
    }
    else
    {
        cmd = IP_ADD_MEMBERSHIP;
        opt.gr4.imr_multiaddr = grp->sin_addr;
        opt.gr4.imr_interface = id;
        optlen = sizeof (opt.gr4);
    }

    msg_Dbg (obj, "IP_ADD_%sMEMBERSHIP multicast request",
             (src != NULL) ? "SOURCE_" : "");

    if (setsockopt (fd, SOL_IP, cmd, &opt, optlen) == 0)
        return 0;

error:
#endif

    msg_Err (obj, "cannot join IPv4 multicast group (%s)",
             net_strerror (net_errno));
    return -1;
}


static int
net_IPv6Join (vlc_object_t *obj, int fd, const struct sockaddr_in6 *src)
{
#ifdef IPV6_JOIN_GROUP
    struct ipv6_mreq gr6;
    memset (&gr6, 0, sizeof (gr6));
    gr6.ipv6mr_interface = src->sin6_scope_id;
    memcpy (&gr6.ipv6mr_multiaddr, &src->sin6_addr, 16);

    msg_Dbg (obj, "IPV6_JOIN_GROUP multicast request");

    if (!setsockopt (fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &gr6, sizeof (gr6)))
        return 0;
#else
    errno = ENOSYS;
#endif

    msg_Err (obj, "cannot join IPv6 any-source multicast group (%s)",
             net_strerror (net_errno));
    return -1;
}


#if 0
#if defined (WIN32) && !defined (MCAST_JOIN_SOURCE_GROUP)
/*
 * I hate manual definitions: Error-prone. Portability hell.
 * Developers shall use UP-TO-DATE compilers. Full point.
 * If you remove the warning, you remove the whole ifndef.
 */
#  warning Your C headers are out-of-date. Please update.

/* No, I won't guess the layout of these two.
 * No, I don't want another socket protection level type-of obnoxious bug. */
#  error Hmmm? This needs fixing.
#  define MCAST_JOIN_GROUP XXX
struct group_req
{
    uint32_t gr_interface; FIXME
    struct sockaddr_storage gr_group; FIXME
};


#  define MCAST_JOIN_SOURCE_GROUP 45 /* from <ws2ipdef.h> */
struct group_source_req
{
    uint32_t gsr_interface;
    struct sockaddr_storage gsr_group;
    struct sockaddr_storage gsr_source;
};
#endif
#endif

/**
 * IP-agnostic multicast join,
 * with fallback to old APIs, and fallback from SSM to ASM.
 */
static int
net_SourceSubscribe (vlc_object_t *obj, int fd,
                     const struct sockaddr *src, socklen_t srclen,
                     const struct sockaddr *grp, socklen_t grplen)
{
    int level, iid = 0;

    char *iface = var_CreateGetString (obj, "miface");
    if (iface != NULL)
    {
        if (*iface)
        {
            iid = if_nametoindex (iface);
            if (iid == 0)
            {
                msg_Err (obj, "invalid multicast interface: %s", iface);
                free (iface);
                return -1;
            }
        }
        free (iface);
    }

    switch (grp->sa_family)
    {
#ifdef AF_INET6
        case AF_INET6:
            level = SOL_IPV6;
            if ((src != NULL)
             && memcmp (&((const struct sockaddr_in6 *)src)->sin6_addr,
                        &in6addr_any, sizeof (in6addr_any)) == 0)
                src = NULL;

            if (((const struct sockaddr_in6 *)src)->sin6_scope_id)
                iid = ((const struct sockaddr_in6 *)src)->sin6_scope_id;
            break;
#endif

        case AF_INET:
            level = SOL_IP;
            if ((src != NULL)
             && ((const struct sockaddr_in *)src)->sin_addr.s_addr
                  == INADDR_ANY)
                src = NULL;
            break;

        default:
            errno = EAFNOSUPPORT;
            return -1;
    }

    /* Agnostic ASM/SSM multicast join */
#ifdef MCAST_JOIN_SOURCE_GROUP
    union
    {
        struct group_req gr;
        struct group_source_req gsr;
    } opt;
    socklen_t optlen;

    memset (&opt, 0, sizeof (opt));

    if (src != NULL)
    {
        if ((grplen > sizeof (opt.gsr.gsr_group))
         || (srclen > sizeof (opt.gsr.gsr_source)))
            return -1;

        opt.gsr.gsr_interface = iid;
        memcpy (&opt.gsr.gsr_source, src, srclen);
        memcpy (&opt.gsr.gsr_group,  grp, grplen);
        optlen = sizeof (opt.gsr);
    }
    else
    {
        if (grplen > sizeof (opt.gr.gr_group))
            return -1;

        opt.gr.gr_interface = iid;
        memcpy (&opt.gr.gr_group, grp, grplen);
        optlen = sizeof (opt.gr);
    }

    msg_Dbg (obj, "Multicast %sgroup join request", src ? "source " : "");

    if (setsockopt (fd, level, 
                    src ? MCAST_JOIN_SOURCE_GROUP : MCAST_JOIN_GROUP,
                    (void *)&opt, optlen) == 0)
        return 0;
#endif

    /* Fallback to IPv-specific APIs */
    if ((src != NULL) && (src->sa_family != grp->sa_family))
        return -1;

    switch (grp->sa_family)
    {
        case AF_INET:
            if ((grplen < sizeof (struct sockaddr_in))
             || ((src != NULL) && (srclen < sizeof (struct sockaddr_in))))
                return -1;

            if (net_IPv4Join (obj, fd, (const struct sockaddr_in *)src,
                              (const struct sockaddr_in *)grp) == 0)
                return 0;
            break;

#ifdef AF_INET6
        case AF_INET6:
            if ((grplen < sizeof (struct sockaddr_in6))
             || ((src != NULL) && (srclen < sizeof (struct sockaddr_in6))))
                return -1;

            /* We don't provide IPv6-specific SSM at the moment.
             * It seems all the OSes with IPv6 SSM have the new API anyway. */

            if (net_IPv6Join (obj, fd, (const struct sockaddr_in6 *)grp) == 0)
                return 0;
            break;
#endif
    }

    msg_Err (obj, "Multicast group join error (%s)",
             net_strerror (net_errno));

    if (src != NULL)
    {
        msg_Warn (obj, "Trying ASM instead of SSM...");
        return net_Subscribe (obj, fd, grp, grplen);
    }

    msg_Err (obj, "Multicast not supported");
    return -1;
}


int net_Subscribe (vlc_object_t *obj, int fd,
                   const struct sockaddr *addr, socklen_t addrlen)
{
    return net_SourceSubscribe (obj, fd, NULL, 0, addr, addrlen);
}


int net_SetDSCP( int fd, uint8_t dscp )
{
    struct sockaddr_storage addr;
    if( getsockname( fd, (struct sockaddr *)&addr, &(socklen_t){ sizeof (addr) }) )
        return -1;

    int level, cmd;

    switch( addr.ss_family )
    {
#ifdef IPV6_TCLASS
        case AF_INET6:
            level = SOL_IPV6;
            cmd = IPV6_TCLASS;
            break;
#endif

        case AF_INET:
            level = SOL_IP;
            cmd = IP_TOS;
            break;

        default:
#ifdef ENOPROTOOPT
            errno = ENOPROTOOPT;
#endif
            return -1;
    }

    return setsockopt( fd, level, cmd, &(int){ dscp }, sizeof (int));
}


/*****************************************************************************
 * __net_ConnectUDP:
 *****************************************************************************
 * Open a UDP socket to send data to a defined destination, with an optional
 * hop limit.
 *****************************************************************************/
int __net_ConnectUDP( vlc_object_t *p_this, const char *psz_host, int i_port,
                      int i_hlim )
{
    struct addrinfo hints, *res, *ptr;
    int             i_val, i_handle = -1;
    vlc_bool_t      b_unreach = VLC_FALSE;

    if( i_port == 0 )
        i_port = 1234; /* historical VLC thing */

    if( i_hlim < 1 )
        i_hlim = var_CreateGetInteger( p_this, "ttl" );

    memset( &hints, 0, sizeof( hints ) );
    hints.ai_socktype = SOCK_DGRAM;

    msg_Dbg( p_this, "net: connecting to %s port %d", psz_host, i_port );

    i_val = vlc_getaddrinfo( p_this, psz_host, i_port, &hints, &res );
    if( i_val )
    {
        msg_Err( p_this, "cannot resolve %s port %d : %s", psz_host, i_port,
                 vlc_gai_strerror( i_val ) );
        return -1;
    }

    for( ptr = res; ptr != NULL; ptr = ptr->ai_next )
    {
        char *str;
        int fd = net_Socket (p_this, ptr->ai_family, ptr->ai_socktype,
                             ptr->ai_protocol);
        if (fd == -1)
            continue;

#if !defined( SYS_BEOS )
        /* Increase the receive buffer size to 1/2MB (8Mb/s during 1/2s)
        * to avoid packet loss caused by scheduling problems */
        setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &(int){ 0x80000 }, sizeof (int));
        setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &(int){ 0x80000 }, sizeof (int));

        /* Allow broadcast sending */
        setsockopt (fd, SOL_SOCKET, SO_BROADCAST, &(int){ 1 }, sizeof (int));
#endif

        if( i_hlim > 0 )
            net_SetMcastHopLimit( p_this, fd, ptr->ai_family, i_hlim );

        str = var_CreateGetString (p_this, "miface");
        if (str != NULL)
        {
            if (*str)
                net_SetMcastOut (p_this, fd, ptr->ai_family, str, NULL);
            free (str);
        }

        str = var_CreateGetString (p_this, "miface-addr");
        if (str != NULL)
        {
            if (*str)
                net_SetMcastOut (p_this, fd, ptr->ai_family, NULL, str);
            free (str);
        }

        net_SetDSCP (fd, var_CreateGetInteger (p_this, "dscp"));

        if( connect( fd, ptr->ai_addr, ptr->ai_addrlen ) == 0 )
        {
            /* success */
            i_handle = fd;
            break;
        }

#if defined( WIN32 ) || defined( UNDER_CE )
        if( WSAGetLastError () == WSAENETUNREACH )
#else
        if( errno == ENETUNREACH )
#endif
            b_unreach = VLC_TRUE;
        else
        {
            msg_Warn( p_this, "%s port %d : %s", psz_host, i_port,
                      strerror( errno ) );
            net_Close( fd );
            continue;
        }
    }

    vlc_freeaddrinfo( res );

    if( i_handle == -1 )
    {
        if( b_unreach )
            msg_Err( p_this, "Host %s port %d is unreachable", psz_host,
                     i_port );
        return -1;
    }

    return i_handle;
}


/*****************************************************************************
 * __net_OpenUDP:
 *****************************************************************************
 * Open a UDP connection and return a handle
 *****************************************************************************/
int __net_OpenUDP( vlc_object_t *obj, const char *psz_bind, int i_bind,
                   const char *psz_server, int i_server )
{
    struct addrinfo hints, *loc, *rem;
    int val;

    msg_Dbg( obj, "net: connecting to '[%s]:%d@[%s]:%d'",
             psz_server, i_server, psz_bind, i_bind );

    memset (&hints, 0, sizeof (hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    val = vlc_getaddrinfo (obj, psz_server, i_server, &hints, &rem);
    if (val)
    {
        msg_Err (obj, "cannot resolve %s port %d : %s", psz_bind, i_bind,
                 vlc_gai_strerror (val));
        return -1;
    }

    val = vlc_getaddrinfo (obj, psz_bind, i_bind, &hints, &loc);
    if (val)
    {
        msg_Err (obj, "cannot resolve %s port %d : %s", psz_bind, i_bind,
                 vlc_gai_strerror (val));
        vlc_freeaddrinfo (rem);
        return -1;
    }

    for (struct addrinfo *ptr = loc; ptr != NULL; ptr = ptr->ai_next)
    {
        int fd = net_Socket (obj, ptr->ai_family, ptr->ai_socktype,
                             ptr->ai_protocol);
        if (fd == -1)
            continue; // usually, address family not supported

#ifdef SO_REUSEPORT
        setsockopt (fd, SOL_SOCKET, SO_REUSEPORT, &(int){ 1 }, sizeof (int));
#endif

#ifdef SO_RCVBUF
        /* Increase the receive buffer size to 1/2MB (8Mb/s during 1/2s)
         * to avoid packet loss caused in case of scheduling hiccups */
        setsockopt (fd, SOL_SOCKET, SO_RCVBUF,
                    (void *)&(int){ 0x80000 }, sizeof (int));
        setsockopt (fd, SOL_SOCKET, SO_SNDBUF,
                    (void *)&(int){ 0x80000 }, sizeof (int));
#endif

#if defined (WIN32) || defined (UNDER_CE)
        if (net_SockAddrIsMulticast (ptr->ai_addr, ptr->ai_addrlen)
         && (sizeof (struct sockaddr_storage) >= ptr->ai_addrlen))
        {
            // This works for IPv4 too - don't worry!
            struct sockaddr_in6 dumb =
            {
                .sin6_family = ptr->ai_addr->sa_family,
                .sin6_port =  ((struct sockaddr_in *)(ptr->ai_addr))->sin_port
            };

            bind (fd, (struct sockaddr *)&dumb, ptr->ai_addrlen);
        }
        else
#endif
        if (bind (fd, ptr->ai_addr, ptr->ai_addrlen))
        {
            net_Close (fd);
            continue;
        }

        struct addrinfo *ptr2;
        for (ptr2 = rem; ptr2 != NULL; ptr2 = ptr2->ai_next)
        {
            if ((ptr2->ai_family != ptr->ai_family)
             || (ptr2->ai_socktype != ptr->ai_socktype)
             || (ptr2->ai_protocol != ptr->ai_protocol))
                continue;

            if (net_SockAddrIsMulticast (ptr->ai_addr, ptr->ai_addrlen))
            {
                if (net_SourceSubscribe (obj, fd,
                                         ptr2->ai_addr, ptr2->ai_addrlen,
                                         ptr->ai_addr, ptr->ai_addrlen) == 0)
                    break;
            }
            else
            {
                if (connect (fd, ptr2->ai_addr, ptr2->ai_addrlen) == 0)
                    break;
            }
        }

        if (ptr2 == NULL)
        {
            msg_Err (obj, "cannot connect to %s port %d: %s",
                     psz_server, i_server, net_strerror (net_errno));
            close (fd);
            continue;
        }

        return fd;
    }

    return -1;
}
