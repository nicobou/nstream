/* ------------------------ nstream~ ------------------------------------------ */
/*                                                                              */
/* Tilde object to receive uncompressed audio data from nstream~.               */
/* Written by Nicolas Bouillot <nicolas@cim.mcgill.ca>                          */
/* Compatibility with PDa: pd for embeded devices                               */
/* Based on netsend~ by Olaf Matthes                                            */
/* witch was based on streamout~ by Guenter Geiger.                             */
/*                                                                              */
/*                                                                              */
/* This program is free software; you can redistribute it and/or                */
/* modify it under the terms of the GNU General Public License                  */
/* as published by the Free Software Foundation; either version 2               */
/* of the License, or (at your option) any later version.                       */
/*                                                                              */
/* See file LICENSE for further informations on licensing terms.                */
/*                                                                              */
/* This program is distributed in the hope that it will be useful,              */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of               */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                */
/* GNU General Public License for more details.                                 */
/*                                                                              */
/* You should have received a copy of the GNU General Public License            */
/* along with this program; if not, write to the Free Software                  */
/* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.  */
/*                                                                              */
/* Based on PureData by Miller Puckette and others.                             */
/*                                                                              */
/*                                                                              */
/* ---------------------------------------------------------------------------- */


#ifdef PD
#include "m_pd.h"

#ifdef BUILD_ASMOBILE
	#include "m_fixed.h"
#endif

#else
#include "ext.h"
#include "z_dsp.h"
#endif

#include "nstream~.h"



#include <sys/types.h>
#include <string.h>
#ifndef _WINDOWS
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#define SOCKET_ERROR -1
#else
#include <winsock.h>
#endif

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

#ifdef NT
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#define DEFAULT_AUDIO_BUFFER_FRAMES 32	/* a small circ. buffer for 32 frames */
#define DEFAULT_AVERAGE_NUMBER 10		/* number of values we store for average history */
#define DEFAULT_NETWORK_POLLTIME 1		/* interval in ms for polling for input data (Max/MSP only) */
#define DEFAULT_QUEUE_LENGTH 3			/* min. number of buffers that can be used reliably on your hardware */


#ifndef _WINDOWS
#define CLOSESOCKET(fd) close(fd)
#endif
#ifdef _WINDOWS
#define CLOSESOCKET(fd) closesocket(fd)
#endif

#ifdef PD
/* these would require to include some headers that are different
   between pd 0.36 and later, so it's easier to do it like this! */
EXTERN void sys_rmpollfn(int fd);
EXTERN void sys_addpollfn(int fd, void* fn, void *ptr);
#endif

static int nsreceive_tilde_sockerror(char *s)
{
#ifdef NT
    int err = WSAGetLastError();
    if (err == 10054) return 1;
    else if (err == 10040) post("nstream~: %s: message too long (%d)", s, err);
    else if (err == 10053) post("nstream~: %s: software caused connection abort (%d)", s, err);
    else if (err == 10055) post("nstream~: %s: no buffer space available (%d)", s, err);
    else if (err == 10060) post("nstream~: %s: connection timed out (%d)", s, err);
    else if (err == 10061) post("nstream~: %s: connection refused (%d)", s, err);
    else post("nsreceive~: %s: %s (%d)", s, strerror(err), err);
#else
    int err = errno;
    post("nsreceive~: %s: %s (%d)", s, strerror(err), err);
#endif
#ifdef NT
	if (err == WSAEWOULDBLOCK)
#endif
#ifdef UNIX
	if (err == EAGAIN)
#endif
	{
		return 1;	/* recoverable error */
	}
	return 0;	/* indicate non-recoverable error */
}



/* ------------------------ nsreceive~ ----------------------------- */


static t_class *nsreceive_tilde_class;
static t_symbol *ps_format, *ps_channels, *ps_framesize, *ps_overflow, *ps_underflow,
                *ps_queuesize, *ps_average, *ps_sf_float, *ps_sf_16bit, *ps_sf_8bit, 
                *ps_sf_mp3,  *ps_sf_unknown, *ps_bitrate, *ps_hostname, *ps_nothing;
static t_symbol  *ps_localhost;
static t_symbol  *ps_date;
static t_symbol  *ps_avdatathrp;
static t_symbol  *ps_datathrp;
static t_symbol  *ps_avlosses;
static t_symbol  *ps_losses;
static t_symbol  *ps_jitter;
static t_symbol  *ps_blocksize;


typedef struct _nsreceive_tilde
{
#ifdef PD
	t_object x_obj;
	t_outlet *x_outlet1;
	t_outlet *x_outlet2;
#else
	t_pxobject x_obj;
	void *x_outlet1;
	void *x_outlet2;
	void *x_connectpoll;
	void *x_datapoll;
#endif
	int x_socket;
	int x_connectsocket;
	int x_nconnections;
	int x_ndrops;
	
        int x_portno;
        t_symbol *x_mcastaddress;
	t_symbol *x_hostname;

	/* buffering */
	int x_framein;
	int x_frameout;
	t_frame x_frames[DEFAULT_AUDIO_BUFFER_FRAMES];
	int x_maxframes;
	long x_framecount;
	int x_blocksize;
	int x_blocksperrecv;
	int x_blockssincerecv;
        int x_lastmallocblocksize;
        //stats
        long x_blockduration; //in usec
        long x_loopduration;
        long x_jittermin;
        long x_jittermax;
      
        long x_datebegin;  
        long x_lastdate;
        long x_lastusecdate;
        int x_lastcounter;
        int x_lost;
        int x_lastlost;
        int x_lastnumber;
        int x_loopcounter;
        int x_counter; //count the number of messages received
	int x_average[DEFAULT_AVERAGE_NUMBER];
	int x_averagecur;
	int x_underflow;
	int x_overflow;


	long x_samplerate;
	int x_noutlets;
	int x_vecsize;
	t_int **x_myvec;            /* vector we pass on to the DSP routine */
} t_nsreceive_tilde;



static int nsreceive_tilde_setsocketoptions(t_nsreceive_tilde *x, int sockfd)
{ 
 
  
  if(x->x_mcastaddress != ps_localhost)
    {
 
      struct sockaddr_in sockaddr_group;
      struct hostent *group;
      struct ip_mreq mreq;
      
      bzero(&mreq,sizeof(struct ip_mreq));
 
      
      // set group
      
      if ((group = gethostbyname(x->x_mcastaddress->s_name))==(struct hostent *)0) {
	nsreceive_tilde_sockerror("gethostbyname multicast");
	return (0);
      }

      
      struct in_addr ia;
      bcopy((void*)group->h_addr,
	    (void*)&ia,
	    group->h_length); 
      bcopy(&ia,
	    &mreq.imr_multiaddr.s_addr,
	    sizeof(struct in_addr));

      
      // set interface
      
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);

      
      // do membership call
      
      if (setsockopt(sockfd,
		     IPPROTO_IP,
		     IP_ADD_MEMBERSHIP,
		     &mreq,
		     sizeof(struct ip_mreq)) 
	  == -1) {
	nsreceive_tilde_sockerror("setsockopt multicast"); 
	  return(0);
      }
 
      
      
    }
  else
    {
 
      int sockopt = 1;
      if (setsockopt(sockfd, SOL_IP, TCP_NODELAY, (const char*)&sockopt, sizeof(int)) < 0)
	post("setsockopt NODELAY failed");
      
      sockopt = 1;    
      if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&sockopt, sizeof(int)) < 0)
	post("nsreceive~: setsockopt REUSEADDR failed");
      

      
    }
 
  

  return 0;
}






/* remove all pollfunctions and close socket */
static void nsreceive_tilde_closesocket(t_nsreceive_tilde* x)
{



#ifdef PD
	sys_rmpollfn(x->x_socket);

	outlet_float(x->x_outlet1, 0);

#else

	clock_unset(x->x_datapoll);
	outlet_int(x->x_outlet1, 0);
#endif

	CLOSESOCKET(x->x_socket);

	x->x_socket = -1;
}



#ifdef PD
static void nsreceive_tilde_reset(t_nsreceive_tilde* x, t_floatarg buffer)
#else
static void nsreceive_tilde_reset(t_nsreceive_tilde* x, double buffer)
#endif
{
	int i;
	x->x_counter = 0;

	

	x->x_framein = 0;
	x->x_frameout = 0;
	x->x_datebegin=0;
        x->x_lastdate=0;
	x->x_loopcounter=0;
        x->x_lastusecdate=0;
        x->x_jittermin=0;
	x->x_jittermax=0;
	x->x_lastnumber=0;
	x->x_lastcounter=0;
	x->x_lost=0;
	x->x_lastlost=0;
	x->x_blockssincerecv = 0;
	x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;


	for (i = 0; i < DEFAULT_AVERAGE_NUMBER; i++)
		x->x_average[i] = x->x_maxframes;
	x->x_averagecur = 0;

	if (buffer == 0.0)	/* set default */
		x->x_maxframes = DEFAULT_QUEUE_LENGTH;
	else
	{
		buffer = (float)CLIP((float)buffer, 0., 1.);
		x->x_maxframes = (int)(DEFAULT_AUDIO_BUFFER_FRAMES * buffer);
		x->x_maxframes = CLIP(x->x_maxframes, 1, DEFAULT_AUDIO_BUFFER_FRAMES - 1);
		post("nsreceive~: set buffer to %g (%d frames), %d usec", buffer, x->x_maxframes,x->x_blockduration * x->x_maxframes );
	}
	x->x_underflow = 0;
	x->x_overflow = 0;
}


#define QUEUESIZE (int)((x->x_framein + DEFAULT_AUDIO_BUFFER_FRAMES - x->x_frameout) % DEFAULT_AUDIO_BUFFER_FRAMES)
#define BLOCKOFFSET (x->x_blockssincerecv * x->x_vecsize * x->x_frames[x->x_frameout].tag.channels)

static void nsreceive_tilde_datapoll(t_nsreceive_tilde *x)
{
#ifndef PD
	int ret;
	struct timeval timout;
    fd_set readset;
    timout.tv_sec = 0;
    timout.tv_usec = 0;
    FD_ZERO(&readset);
    FD_SET(x->x_socket, &readset);

	ret = select(x->x_socket + 1, &readset, NULL, NULL, &timout);
    if (ret < 0)
    {
    	nsreceive_tilde_sockerror("select");
		return;
    }

	if (FD_ISSET(x->x_socket, &readset))	/* data available */
#endif
	{
		int ret;

		int nic =0;
		int framein=0;



		/* UDP */
		{
		  //n = x->x_nbytes;

		  //nicnbytesif (x->x_nbytes == 0)	/* we ate all the samples and need a new header tag */
		  //nicnbytes {
				/* receive header tag */
				//ret = recv(x->x_socket,
		  //(char*)&x->x_frames[x->x_framein].tag,
		  //sizeof(t_tag), 0);
		  //post("sizeof(t_tag) %d DEFAULT_CBUF_SIZE %d header %d",sizeof(t_tag),DEFAULT_CBUF_SIZE,sizeof(t_tag),DEFAULT_CBUF_SIZE-sizeof(t_tag));

		  //ret = recv(x->x_socket, (char*)&x->x_frames[x->x_framein].tag, sizeof(t_tag) - DEFAULT_CBUF_SIZE, 0);
				
		  ret = recv(x->x_socket, (char*)&x->x_frames[x->x_framein], sizeof(t_frame), 0);
		
		               if (ret <= 0)	/* error */
				  {
					if (nsreceive_tilde_sockerror("recv tag"))
						goto bail;
					nsreceive_tilde_reset(x, 0);
					x->x_datebegin=0;
					x->x_lastdate=0;
					x->x_lastusecdate=0;
					x->x_jittermin=0;
					x->x_jittermax=0;
					x->x_lastnumber=0;
					x->x_lastcounter=0;
					x->x_loopcounter=0;
					x->x_lost=0;
					x->x_lastlost=0;
					x->x_counter = 0;
					return;
				}
				else if (ret <= sizeof(t_frame) - DEFAULT_CBUF_SIZE)
				{
					/* incomplete header tag: return and try again later */
					/* in the hope that more data will be available */
					error("nsreceive~: got incomplete header tag");
					return;
				}
			       
			       struct timeval tv;
		
			       gettimeofday(&tv, NULL); 

			       if(x->x_datebegin == 0)
				 {
				   x->x_datebegin=tv.tv_sec;
				   
		
				 }
				if(x->x_lastdate==0)
				  {
				    x->x_lastdate= (long) tv.tv_sec;
				    x->x_lastusecdate=tv.tv_usec;
				    x->x_jittermin=0;
				    x->x_jittermax=0;
				    x->x_lastcounter=0;
				    x->x_loopcounter=0;
				    x->x_lastnumber=x->x_frames[x->x_framein].tag.count;
				    x->x_lastlost=0;

				  }
			
			       /* adjust byte order if neccessarry headers are sent using little endian format*/
				//	if ( SF_BYTE_NATIVE ==  SF_BYTE_BE && x->x_frames[x->x_framein].tag.version != SF_BYTE_LE )//x->x_frames[x->x_framein].tag.version == SF_BYTE_BE)
				if ( x->x_frames[x->x_framein].tag.version != SF_BYTE_LE )
				{
					x->x_frames[x->x_framein].tag.count = toles(x->x_frames[x->x_framein].tag.count);
					x->x_frames[x->x_framein].tag.framesize = toles(x->x_frames[x->x_framein].tag.framesize);
				}


				/* get info from header tag */
				if (x->x_frames[x->x_framein].tag.channels > x->x_noutlets)
				{
					error("nsreceive~: incoming stream has too many channels (%d)", x->x_frames[x->x_framein].tag.channels);
					x->x_datebegin=0;
					x->x_lastdate=0;
					x->x_lastusecdate=0;
					x->x_jittermin=0;
					x->x_jittermax=0;
					x->x_lastnumber=0;
					x->x_lastcounter=0;
					x->x_loopcounter=0;
					x->x_lost=0;
					x->x_lastlost=0;
					x->x_counter = 0;
					return;
				}
		

				/* check whether the data packet has the correct count */
				if ((x->x_framecount != x->x_frames[x->x_framein].tag.count)
				    && (x->x_frames[x->x_framein].tag.count > 100)
				    && (x->x_framecount != 0 ) )
				{
		
					
		
				  if(x->x_framecount < x->x_frames[x->x_framein].tag.count)
				    {
				      x->x_lost += (int)(x->x_frames[x->x_framein].tag.count - x->x_framecount);
				      x->x_lastlost += (int)(x->x_frames[x->x_framein].tag.count - x->x_framecount);
				    }
				  else //data arrive out of order
				    {
				      post("nsreceive~: out of order data received");
				      goto bail;
				    }
				}
				x->x_framecount = x->x_frames[x->x_framein].tag.count + 1;
				
				
				int nbsample =x->x_frames[x->x_framein].tag.framesize / ( SF_SIZEOF(x->x_frames[x->x_framein].tag.format) * x->x_frames[x->x_framein].tag.channels) ;
				
				

		
				
				if ( x->x_blocksize != nbsample )
				  {
		
				    framein=x->x_framein;
				    x->x_framein=0;
				    x->x_datebegin=0;
				    x->x_lastdate=0;
				    x->x_lastusecdate=0;
				    x->x_lastcounter=0;
				    x->x_loopcounter=0;
				    x->x_lastnumber=0;
				    x->x_frameout=0;
				    x->x_lost=0;
				    x->x_lastlost=0;
				    x->x_counter=0;
		
				    for (nic = 0; nic < DEFAULT_AVERAGE_NUMBER; nic++)
				      x->x_average[nic] = x->x_maxframes;
				    x->x_averagecur = 0;
				    x->x_underflow = 0;
				    x->x_overflow = 0;
				    
				    //freein memory
				    //for (nic = 0; nic <  DEFAULT_AUDIO_BUFFER_FRAMES; nic++)
				    // {
				    //t_freebytes(x->x_frames[nic].data, x->x_blocksize * x->x_noutlets * sizeof(t_float));
				    // }				   
				    
				    //computing new block size
				    x->x_blocksize = x->x_frames[framein].tag.framesize / ( SF_SIZEOF(x->x_frames[framein].tag.format) * x->x_frames[framein].tag.channels );
				    
				    x->x_blockduration= (1000000 * x->x_blocksize) / x->x_samplerate;
				    post("blockduration %d",	x->x_blockduration);  
				    post("nsreceive: changement de blocksize UDP %d",x->x_blocksize);
				    x->x_loopduration= (1000000 * 64) / x->x_samplerate;
				    post("loopduration %d",	x->x_blockduration);  
				    
				    
				    x->x_blockssincerecv = 0;  
				    x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;
				    
				    //cheking pb with max size
				    //nic
				    if(x->x_blocksize * x->x_noutlets * sizeof(t_float) > x->x_lastmallocblocksize )
				      {
					
					//post("nsreceive resizing
					//buffer from %d to %d
					//bytes",x->x_lastmallocblocksize,x->x_blocksize
					//* x->x_noutlets *
					//sizeof(t_float));
					post("receiving framesize to large : %d bytes",x->x_blocksize * x->x_noutlets * sizeof(t_float));
					
					/* for (nic = 0; nic <  DEFAULT_AUDIO_BUFFER_FRAMES; nic++) */
					/* 					  { */
					
/* 					    x->x_frames[nic].data = (char *)t_resizebytes(x->x_frames[nic].data, */
/* 											  x->x_lastmallocblocksize,  */
/* 											  x->x_blocksize  */
/* 											  * x->x_noutlets  */
/* 											  * sizeof(t_float));  */
/* 					  } */
					
/* 					x->x_lastmallocblocksize = x->x_blocksize  */
/* 					  * x->x_noutlets * sizeof(t_float); */
				      }

				    //updating tags for the new frame
				    x->x_frames[x->x_framein].tag.count=x->x_frames[framein].tag.count;
				    x->x_frames[x->x_framein].tag.framesize=x->x_frames[framein].tag.framesize;
				    x->x_frames[x->x_framein].tag.channels=x->x_frames[framein].tag.channels;
				    //x->x_maxframes = DEFAULT_QUEUE_LENGTH;
				    
				  } //end frame size update




				{
				  
				  x->x_counter++;
				  x->x_lastcounter++;
				  
				  //computing jitter
				  //post("blockduration %d ", x->x_blockduration);
				  //	long jit =  (x->x_lastlost + x->x_lastcounter) * x->x_blockduration
				  //- ( 1000000 * (tv.tv_sec - x->x_lastdate) + tv.tv_usec - x->x_lastusecdate ) ;
				  //long jit =  (x->x_frames[x->x_framein].tag.count - x->x_lastnumber ) * x->x_blockduration
				  //- ( 1000000 * (tv.tv_sec - x->x_lastdate) + tv.tv_usec - x->x_lastusecdate ) ;
				  
				  //using only sound card clock (more accurate)
				  //soustraction du temps coorespondant aux paquets recus moins celui correspondant au paquets lus
				  long jit =  (x->x_frames[x->x_framein].tag.count - x->x_lastnumber ) * x->x_blockduration
				    - ( x->x_loopduration * x->x_loopcounter ) ;
				  //post("duree bloc %d duree syst %d diff %d",(x->x_lastlost + x->x_lastcounter) * x->x_blockduration, 1000000 * (tv.tv_sec - x->x_lastdate) + tv.tv_usec - x->x_lastusecdate, jit );
				  if(jit < x->x_jittermin) 
				    {
				      x->x_jittermin = jit;
				      // post("min: jit %d, lastcout %d",jit,x->x_lastcounter);
				    }
				  if(jit > x->x_jittermax) 
				    {
				      x->x_jittermax = jit; 
				      //post("max: jit %d, lastcout %d",jit,x->x_lastcounter);
				    }			
				  
				  //clock skew hiding
				  if(QUEUESIZE < 2 * x->x_maxframes)
				    {
				      x->x_framein++;
				      x->x_framein %= DEFAULT_AUDIO_BUFFER_FRAMES;
				    }
				  else
				    {
				      x->x_overflow++;
				    }
				  
				  /* check for buffer overflow */
				  if (x->x_framein == x->x_frameout)
				    {
				      x->x_overflow++;
				    }
				}

		}
	}
 bail:
	;
#ifndef PD
	clock_delay(x->x_datapoll, DEFAULT_NETWORK_POLLTIME);
#endif
}

static void nsreceive_tilde_connectpoll(t_nsreceive_tilde *x)
{
#ifndef PD
	int ret;
    struct timeval timout;
    fd_set readset;
    timout.tv_sec = 0;
    timout.tv_usec = 0;
    FD_ZERO(&readset);
    FD_SET(x->x_connectsocket, &readset);

	ret = select(x->x_connectsocket + 1, &readset, NULL, NULL, &timout);
    if (ret < 0)
    {
    	nsreceive_tilde_sockerror("select");
		return;
    }

	if (FD_ISSET(x->x_connectsocket, &readset))	/* pending connection */
#endif
	{
		int sockaddrl = (int)sizeof(struct sockaddr);
		struct sockaddr_in incomer_address;
		int fd = accept(x->x_connectsocket, (struct sockaddr*)&incomer_address, &sockaddrl);
		if (fd < 0) 
		{
			post("nsreceive~: accept failed");
			return;
		}
#ifdef O_NONBLOCK
		fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
		if (x->x_socket != -1)
		{
			post("nsreceive~: new connection");
			nsreceive_tilde_closesocket(x);
		}

		nsreceive_tilde_reset(x, 0);
		x->x_socket = fd;
		//x->x_nbytes = 0;
		x->x_hostname = gensym(inet_ntoa(incomer_address.sin_addr));
#ifdef PD
		sys_addpollfn(fd, nsreceive_tilde_datapoll, x);
		outlet_float(x->x_outlet1, 1);
#else
		clock_delay(x->x_datapoll, 0);
		outlet_int(x->x_outlet1, 1);
#endif
	}
#ifndef PD
	clock_delay(x->x_connectpoll, DEFAULT_NETWORK_POLLTIME);
#endif
}


static int nsreceive_tilde_createsocket(t_nsreceive_tilde* x, int portno)
{
    struct sockaddr_in server;
    int sockfd;
    


      sockfd = socket(AF_INET, SOCK_DGRAM, 0);


    if (sockfd < 0)
    {
        nsreceive_tilde_sockerror("socket");
        return 0;
    }
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;

    /* assign server port number */

    server.sin_port = htons((u_short)portno);
    post("listening to port number %d", portno);

    nsreceive_tilde_setsocketoptions(x,sockfd);


    /* name the socket */
    if (bind(sockfd, (struct sockaddr *)&server, sizeof(server)) < 0)
	{
         nsreceive_tilde_sockerror("bind");
         CLOSESOCKET(sockfd);
         return 0;
    }


 
	
         x->x_socket = sockfd;

#ifdef PD
         sys_addpollfn(sockfd, nsreceive_tilde_datapoll, x);
#else
		 clock_delay(x->x_datapoll, 0);
#endif
    return 1;
}



#ifdef PD
static void nsreceive_tilde_receivefrom(t_nsreceive_tilde *x, t_symbol *host, t_floatarg fportno)
#else
static void nsreceive_tilde_receivefrom(t_nsreceive_tilde *x, t_symbol *host, long fportno)
#endif
{
  


  if (host != ps_nothing)
    x->x_mcastaddress = host;
  else
    x->x_mcastaddress = ps_localhost;
  
  if (!fportno)
    x->x_portno = DEFAULT_PORT;
  else
    x->x_portno = (int)fportno;


  if (x->x_socket != -1)
    {
    post("x_socket = %d x_connectsoc %d",x->x_socket,x->x_connectsocket);

#ifdef PD
    sys_rmpollfn(x->x_socket);

#else
    clock_unset(x->x_datapoll);

#endif
    CLOSESOCKET(x->x_socket); 

    x->x_socket = -1;

    } 
  nsreceive_tilde_createsocket(x, fportno);
}









static t_int *nsreceive_tilde_perform(t_int *w)
{
	t_nsreceive_tilde *x = (t_nsreceive_tilde*) (w[1]);
	int n = (int)(w[2]);
	//t_float *out[DEFAULT_AUDIO_CHANNELS];
	t_sample *out[DEFAULT_AUDIO_CHANNELS];
	const int offset = 3;
	const int channels = x->x_frames[x->x_frameout].tag.channels;
	int i = 0;

	x->x_loopcounter++;

	for (i = 0; i < x->x_noutlets; i++)
	{
	  //out[i] = (t_float *)(w[offset + i]);
	  out[i] = (t_sample *)(w[offset + i]);
	}

	if (n != x->x_vecsize)
	{


	  post("nsreceive: convertion de vectsize");

		x->x_vecsize = n;
		x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;
		x->x_blockssincerecv = 0;
	}

	/* to start reading after initialisation, check whether there is enough data in buffer */
	if (x->x_counter < x->x_maxframes)
	{

	  goto bail;
	}
	
	/* check for buffer underflow */
	if (x->x_framein == x->x_frameout)
	  {
	    x->x_underflow++;

	    goto bail;
	  }
	

	/* queue balancing */
	x->x_average[x->x_averagecur] = QUEUESIZE;
	if (++x->x_averagecur >= DEFAULT_AVERAGE_NUMBER)
		x->x_averagecur = 0;

	switch (x->x_frames[x->x_frameout].tag.format)
	{
		case SF_FLOAT:
		{

		  //t_float* buf = (t_float *)x->x_frames[x->x_frameout].tag.cbuf + BLOCKOFFSET;
		  t_sample* buf = (t_sample *)x->x_frames[x->x_frameout].tag.cbuf + BLOCKOFFSET;

			if (x->x_frames[x->x_frameout].tag.version == SF_BYTE_NATIVE)
			{
				while (n--)
				{
					for (i = 0; i < channels; i++)
					{     
						*out[i]++ = *buf++;
					}
					for (i = channels; i < x->x_noutlets; i++)
					{     
						*out[i]++ = 0.;
					}
				}
			}
			else	/* swap bytes */
			{
				while (n--)
				{
					for (i = 0; i < channels; i++)
					{     
						*out[i]++ = nstream_float(*buf++);
					}
					for (i = channels; i < x->x_noutlets; i++)
					{     
						*out[i]++ = 0.;
					}
				}
			}
			break;
		}
		case SF_16BIT:     
		{
			short* buf = (short *)x->x_frames[x->x_frameout].tag.cbuf + BLOCKOFFSET;

			//if (x->x_frames[x->x_frameout].tag.version == SF_BYTE_SF_BYTE_LE)
			{
				while (n--)
				{
					for (i = 0; i < channels; i++)
					{

					  
#ifdef FIXEDPOINT
					  *out[i]++ = (t_sample)INVSCALE16(*buf++);
#else
					  *out[i]++ = (t_float)(*buf++ * 3.051850e-05);
#endif
					  
					}
					

					for (i = channels; i < x->x_noutlets; i++)
					{
						*out[i]++ = 0.;
					}
				}
			}
/* 			else /\* swap bytes *\/ */
/* 			{ */
/* 				while (n--) */
/* 				{ */
/* 					for (i = 0; i < channels; i++) */
/* 					{ */
/* 					  *out[i]++ = (t_float)(nstream_short(*buf++) * 3.051850e-05); */
/* 					} */
/* 					for (i = channels; i < x->x_noutlets; i++) */
/* 					{ */
/* 						*out[i]++ = 0.; */
/* 					} */
/* 				} */
/* 			} */
			break;
		}
		case SF_8BIT:     
		{
			char* buf = (char *)x->x_frames[x->x_frameout].tag.cbuf + BLOCKOFFSET;

			while (n--)
			{
				for (i = 0; i < channels; i++)
				{ 
	 			  
#ifdef FIXEDPOINT				 
				  *out[i]++ = (t_sample)INVSCALE8(*buf++) - 1.;
#else
				  *out[i]++ = (t_sample)((0.0078125 * (*buf++)) - 1.0);
#endif	 
				}
		    		for (i = channels; i < x->x_noutlets; i++)
				{
					*out[i]++ = 0.;
				}
			}
			break;
		}
		case SF_MP3:     
		{
			post("nsreceive~: mp3 format not supported");
			
		}
		default:
			post("nsreceive~: unknown format (%d)",x->x_frames[x->x_frameout].tag.format);
			
			break;
	}

	if (!(x->x_blockssincerecv < x->x_blocksperrecv - 1))
	{
		x->x_blockssincerecv = 0;
		x->x_frameout++;
		x->x_frameout %= DEFAULT_AUDIO_BUFFER_FRAMES;
	}
	else
	{
		x->x_blockssincerecv++;
	}

	return (w + offset + x->x_noutlets);

bail:
	/* set output to zero */
	while (n--)
	{
		for (i = 0; i < x->x_noutlets; i++)
		{
			*(out[i]++) = 0.;
		}
	}
	return (w + offset + x->x_noutlets);
}



static void nsreceive_tilde_dsp(t_nsreceive_tilde *x, t_signal **sp)
{
	int i;

	x->x_myvec[0] = (t_int*)x;
	x->x_myvec[1] = (t_int*)sp[0]->s_n;

	x->x_samplerate = (long)sp[0]->s_sr;
	if(x->x_blockduration == 0) x->x_blockduration = (1000000 * x->x_blocksize) / x->x_samplerate ;
	if(x->x_loopduration == 0) x->x_loopduration = (1000000 * 64) / x->x_samplerate ;


	post("samplerate %d, blockduration %d",x->x_samplerate,x->x_blockduration );
	post("samplerate %d, loopduration %d",x->x_samplerate,x->x_loopduration );
	
	if (DEFAULT_AUDIO_BUFFER_SIZE % sp[0]->s_n)
	{
		error("nstream~: signal vector size too large (needs to be even divisor of %d)", DEFAULT_AUDIO_BUFFER_SIZE);
	}
	else
	{
#ifdef PD
		for (i = 0; i < x->x_noutlets; i++)
		{
			x->x_myvec[2 + i] = (t_int*)sp[i + 1]->s_vec;
		}
		dsp_addv(nsreceive_tilde_perform, x->x_noutlets + 2, (t_int*)x->x_myvec);
#else
		for (i = 0; i < x->x_noutlets; i++)
		{
			x->x_myvec[2 + i] = (t_int*)sp[i]->s_vec;
		}
		dsp_addv(nsreceive_tilde_perform, x->x_noutlets + 2, (void **)x->x_myvec);
#endif	/* PD */
	}
}


/* send stream info when banged */
static void nsreceive_tilde_bang(t_nsreceive_tilde *x)
{
 	t_atom list[2]; 
 	t_symbol *sf_format; 
 	t_float bitrate; 
 	int i, avg = 0; 
 	for (i = 0; i < DEFAULT_AVERAGE_NUMBER; i++) 
 		avg += x->x_average[i]; 

	

 	bitrate = (t_float)((SF_SIZEOF(x->x_frames[x->x_frameout].tag.format) * x->x_samplerate * 8 * x->x_frames[x->x_frameout].tag.channels) / 1000.); 

	

 	switch (x->x_frames[x->x_frameout].tag.format) 
 	{ 
 		case SF_FLOAT: 
 		{ 
 			sf_format = ps_sf_float; 
 			break; 
 		} 
 		case SF_16BIT: 
 		{ 
 			sf_format = ps_sf_16bit; 
 			break; 
 		} 
 		case SF_8BIT: 
 		{ 
 			sf_format = ps_sf_8bit; 
 			break; 
 		} 
 		case SF_MP3: 
 		{ 
 			sf_format = ps_sf_mp3; 
 			break; 
 		} 
 		default: 
 		{ 
 			sf_format = ps_sf_unknown; 
 			break; 
 		} 
 	} 

#ifdef PD

	
 	/* --- stream information (t_tag) --- */ 
 	/* audio format */ 
 	SETSYMBOL(list, (t_symbol *)sf_format); 
 	outlet_anything(x->x_outlet2, ps_format, 1, list); 

 	/* channels */ 
 	SETFLOAT(list, (t_float)x->x_frames[x->x_frameout].tag.channels); 
 	outlet_anything(x->x_outlet2, ps_channels, 1, list); 

 	/* framesize */ 
 	SETFLOAT(list, (t_float)x->x_frames[x->x_frameout].tag.framesize); 
 	outlet_anything(x->x_outlet2, ps_framesize, 1, list); 

 	/* bitrate */ 
 	SETFLOAT(list, (t_float)bitrate); 
 	outlet_anything(x->x_outlet2, ps_bitrate, 1, list); 

 	/* --- internal info (buffer and network) --- */ 
 	/* overflow */ 
 	SETFLOAT(list, (t_float)x->x_overflow); 
 	outlet_anything(x->x_outlet2, ps_overflow, 1, list); 

 	/* underflow */ 
 	SETFLOAT(list, (t_float)x->x_underflow); 
 	outlet_anything(x->x_outlet2, ps_underflow, 1, list); 

 	/* queuesize */ 
 	SETFLOAT(list, (t_float)QUEUESIZE); 
 	outlet_anything(x->x_outlet2, ps_queuesize, 1, list); 

 	/* average queuesize */ 
 	SETFLOAT(list, (t_float)((t_float)avg / (t_float)DEFAULT_AVERAGE_NUMBER)); 
 	outlet_anything(x->x_outlet2, ps_average, 1, list); 

		
 	SETFLOAT(list, (t_float)x->x_blocksize); 
 	outlet_anything(x->x_outlet2, ps_blocksize, 1, list); 

	char buffer[30]; 
 	
	struct timeval tv; 
 	time_t curtime; 
 	gettimeofday(&tv, NULL);  
 	curtime=tv.tv_sec; 
	
 	if((long) curtime != x->x_datebegin  && x->x_datebegin != 0 && x->x_lastdate != 0) 
 	  { 
  	  //date  
  	    //strftime(buffer,30,"%m-%d-%Y  %T.",localtime(&curtime));  
	   
	    sprintf(buffer,"%ld.%ld",tv.tv_sec,tv.tv_usec);
	    //	    post("%s",buffer);
	    // strftime(buffer,30,"%s",localtime(&curtime));    
	    //post("%ld\n",tv.tv_sec);  
	    //  	   post("%s%ld\n",buffer,tv.tv_usec);  
  	    //post("usec %ld\n",tv.tv_usec+ 1000000 * (tv.tv_sec - x->x_datebegin));  
  	    //post("%s\n",buffer,tv.tv_usec);  
   	    t_symbol *date = gensym(buffer);   
   	    SETSYMBOL(list, (t_symbol *)date);   
   	    outlet_anything(x->x_outlet2, ps_date, 1, list);   
	  
 	     //average data throughput (without headers) in kbits/s  
  	    t_float avdatathroughput = (x->x_frames[x->x_framein].tag.framesize * 8 * x->x_counter)   
  	      / (( curtime - x->x_datebegin) * 1000.) ;  
  	    SETFLOAT(list, (t_float) avdatathroughput);  
  	    outlet_anything(x->x_outlet2, ps_avdatathrp, 1, list);  

  	    //data throughput (without headers) since last bang in kbits/s  
  	    t_float datathroughput = (x->x_frames[x->x_framein].tag.framesize * 8 * x->x_lastcounter)   
  	      / (( curtime - x->x_lastdate) * 1000.) ;  
  	    SETFLOAT(list, (t_float) datathroughput);  
  	    outlet_anything(x->x_outlet2, ps_datathrp, 1, list);  
	    
  	    //network losses since the begining  
  	    t_float avlosses;  
  	    if((x->x_counter + x->x_lost) != 0 )  
  	      avlosses = 100. * x->x_lost / (x->x_counter + x->x_lost);  
  	    else  
  	      avlosses = 0;  
  	    SETFLOAT(list, (t_float) avlosses);  
  	    outlet_anything(x->x_outlet2, ps_avlosses, 1, list);  

  	    //network losses since last bang  
  	    t_float losses;  
  	    if((x->x_lastcounter + x->x_lastlost) != 0 )  
  	      losses = 100. * x->x_lastlost / (x->x_lastcounter + x->x_lastlost);  
  	    else  
  	      losses = 0;  
  	    SETFLOAT(list, (t_float) losses);  
  	    outlet_anything(x->x_outlet2, ps_losses, 1, list);  

	    //late arrival loss


  	    //max jitter (ms)  
  	    t_float jitter = (x->x_jittermax - x->x_jittermin) / 1000.;  
  	    SETFLOAT(list, (t_float) jitter);  
  	    outlet_anything(x->x_outlet2, ps_jitter, 1, list);  
  	    //	    post("jittermin %d jittermax %d blockduration %d lastcounter %d lastlost %d",x->x_jittermin,x->x_jittermax,x->x_blockduration,x->x_lastcounter,x->x_lastlost);  

  	    x->x_lastdate=0; //updated at next packet, (jittermin, max and lastusecdate also)  

 	  } 


#else


/* 	/\* --- stream information (t_tag) --- *\/ */
/* 	/\* audio format *\/ */
/* 	SETSYM(list, ps_format); */
/* 	SETSYM(list + 1, (t_symbol *)sf_format); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */

/* 	/\* channels *\/ */
/* 	SETSYM(list, ps_channels); */
/* 	SETLONG(list + 1, (int)x->x_frames[x->x_frameout].tag.channels); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */

/* 	/\* framesize *\/ */
/* 	SETSYM(list, ps_framesize); */
/* 	SETLONG(list + 1, (int)x->x_frames[x->x_frameout].tag.framesize); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */

/* 	/\* bitrate *\/ */
/* 	SETSYM(list, ps_bitrate); */
/* 	SETFLOAT(list + 1, (t_float)bitrate); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */

/* 	/\* --- internal info (buffer and network) --- *\/ */
/* 	/\* overflow *\/ */
/* 	SETSYM(list, ps_overflow); */
/* 	SETLONG(list + 1, (int)x->x_overflow); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */

/* 	/\* underflow *\/ */
/* 	SETSYM(list, ps_underflow); */
/* 	SETLONG(list + 1, (int)x->x_underflow); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */

/* 	/\* queuesize *\/ */
/* 	SETSYM(list, ps_queuesize); */
/* 	SETLONG(list + 1, (int)QUEUESIZE); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */

/* 	/\* average queuesize *\/ */
/* 	SETSYM(list, ps_average); */
/* 	SETFLOAT(list + 1, (t_float)((t_float)avg / (t_float)DEFAULT_AVERAGE_NUMBER)); */
/* 	outlet_list(x->x_outlet2, NULL, 2, list); */



#endif
}



static void nsreceive_tilde_print(t_nsreceive_tilde* x)
{
	int i, avg = 0;
	for (i = 0; i < DEFAULT_AVERAGE_NUMBER; i++)
		avg += x->x_average[i];
	post("nsreceive~: last size = %d, avg size = %g, %d underflows, %d overflows", QUEUESIZE, (float)((float)avg / (float)DEFAULT_AVERAGE_NUMBER), x->x_underflow, x->x_overflow);
	post("nsreceive~: channels = %d, framesize = %d, packets = %d", x->x_frames[x->x_framein].tag.channels, x->x_frames[x->x_framein].tag.framesize, x->x_counter);
}



#ifdef PD
static void *nsreceive_tilde_new(t_floatarg fportno, t_floatarg outlets)
#else
static void *nsreceive_tilde_new(long fportno, long outlets)
#endif
{
	t_nsreceive_tilde *x;
	int i;

	if (fportno == 0) fportno = DEFAULT_PORT;

#ifdef PD
	x = (t_nsreceive_tilde *)pd_new(nsreceive_tilde_class);
    if (x)
    { 
        for (i = sizeof(t_object); i < (int)sizeof(t_nsreceive_tilde); i++)  
                ((char *)x)[i] = 0; 
	}

	x->x_noutlets = CLIP((int)outlets, 1, DEFAULT_AUDIO_CHANNELS);
	for (i = 0; i < x->x_noutlets; i++)
		outlet_new(&x->x_obj, &s_signal);
	//if (!prot)
	//	x->x_outlet1 = outlet_new(&x->x_obj, &s_anything);	/* outlet for connection state (TCP/IP) */
	x->x_outlet2 = outlet_new(&x->x_obj, &s_anything);
#else
	x = (t_nsreceive_tilde *)newobject(nsreceive_tilde_class);
    if (x)
    { 
        for (i = sizeof(t_pxobject); i < (int)sizeof(t_nsreceive_tilde); i++)  
                ((char *)x)[i] = 0; 
	}

	dsp_setup((t_pxobject *)x, 0);	/* no signal inlets */
	x->x_noutlets = CLIP((int)outlets, 1, DEFAULT_AUDIO_CHANNELS);
	x->x_outlet2 = listout(x);	/* outlet for info list */
	if (!prot)
		x->x_outlet1 = listout(x);	/* outlet for connection state (TCP/IP) */
	for (i = 0 ; i < x->x_noutlets; i++)
		outlet_new(x, "signal");
	x->x_connectpoll = clock_new(x, (method)nsreceive_tilde_connectpoll);
	x->x_datapoll = clock_new(x, (method)nsreceive_tilde_datapoll);
#endif

	x->x_myvec = (t_int **)t_getbytes(sizeof(t_int *) * (x->x_noutlets + 3));
	if (!x->x_myvec)
	{
		error("nsreceive~: out of memory");
		return NULL;
	}


	x->x_mcastaddress = ps_localhost;
	x->x_portno = 3000;
	x->x_connectsocket = -1;
	x->x_socket = -1;
	x->x_datebegin=0;
	x->x_lastdate=0;
        x->x_lastcounter=0;
	x->x_loopcounter=0;
	x->x_counter=0;
	x->x_framecount=0;
	x->x_lost=0;
	x->x_lastlost=0;
	x->x_nconnections = 0;
	x->x_ndrops = 0;
	x->x_underflow = 0;
	x->x_overflow = 0;
	x->x_hostname = ps_nothing;

	//for (i = 0; i < DEFAULT_AUDIO_BUFFER_FRAMES; i++)
	//{
	  //nic
	  //x->x_frames[i].data = (char
	  //*)t_getbytes(DEFAULT_AUDIO_BUFFER_SIZE * x->x_noutlets *
	  //sizeof(t_float));
	  //x->x_frames[i].data = (char *)t_getbytes(DEFAULT_CBUF_SIZE);
	//}
	x->x_lastmallocblocksize=DEFAULT_CBUF_SIZE;
	x->x_framein = 0;
	x->x_frameout = 0;
	x->x_maxframes = DEFAULT_QUEUE_LENGTH;
	x->x_vecsize = 64;	/* we'll update this later */
	x->x_blocksize = DEFAULT_AUDIO_BUFFER_SIZE;	
	x->x_blockduration = 0;
	x->x_loopduration = 0;
	post("blockduration %d",	x->x_blockduration);
	x->x_blockssincerecv = 0;
	x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;



	if (!nsreceive_tilde_createsocket(x, (int)fportno))
	{
		error("nsreceive~: failed to create listening socket");
		return (NULL);
	}

	return (x);
}



static void nsreceive_tilde_free(t_nsreceive_tilde *x)
{
	int i;

	
	
	if (x->x_connectsocket != -1)
	{
#ifdef PD
		sys_rmpollfn(x->x_connectsocket);
#else
		clock_unset(x->x_connectpoll);
#endif
		CLOSESOCKET(x->x_connectsocket);
	}
	if (x->x_socket != -1)
	{
#ifdef PD
		sys_rmpollfn(x->x_socket);
#else
		clock_unset(x->x_datapoll);
#endif
		CLOSESOCKET(x->x_socket);
	}

#ifndef PD
	dsp_free((t_pxobject *)x);	/* free the object */
	clock_free(x->x_connectpoll);
	clock_free(x->x_datapoll);
#endif


	/* free memory */
	t_freebytes(x->x_myvec, sizeof(t_int *) * (x->x_noutlets + 3));
	for (i = 0; i < DEFAULT_AUDIO_BUFFER_FRAMES; i++)
	{
	  // nic t_freebytes(x->x_frames[i].data,
	  // DEFAULT_AUDIO_BUFFER_SIZE * x->x_noutlets *
	  // sizeof(t_float));
	  //t_freebytes(x->x_frames[i].data, x->x_blocksize * x->x_noutlets * sizeof(t_float));
	  //endnic
	}
}



#ifdef PD
void nsreceive_tilde_setup(void)
{
	nsreceive_tilde_class = class_new(gensym("nsreceive~"), 
		(t_newmethod) nsreceive_tilde_new, (t_method) nsreceive_tilde_free,
		sizeof(t_nsreceive_tilde),  0, A_DEFFLOAT, A_DEFFLOAT, A_NULL);

	class_addmethod(nsreceive_tilde_class, nullfn, gensym("signal"), 0);
	class_addbang(nsreceive_tilde_class, (t_method)nsreceive_tilde_bang);
	class_addmethod(nsreceive_tilde_class, (t_method)nsreceive_tilde_dsp, gensym("dsp"), 0);
	class_addmethod(nsreceive_tilde_class, (t_method)nsreceive_tilde_print, gensym("print"), 0);
	class_addmethod(nsreceive_tilde_class, (t_method)nsreceive_tilde_reset, gensym("reset"), A_DEFFLOAT, 0);
	class_addmethod(nsreceive_tilde_class, (t_method)nsreceive_tilde_reset, gensym("buffer"), A_DEFFLOAT, 0);
	//multicast catching (one source per adress)
	class_addmethod(nsreceive_tilde_class, (t_method)nsreceive_tilde_receivefrom, gensym("connect"), A_DEFSYM, A_DEFFLOAT, 0);
	class_sethelpsymbol(nsreceive_tilde_class, gensym("nstream~"));



	
	ps_localhost = gensym("localhost");
	ps_date = gensym("date");
	ps_avdatathrp = gensym("avdatathrp");
	ps_datathrp = gensym("datathrp");
	ps_avlosses= gensym("avlosses");
	ps_losses= gensym("losses");
	ps_jitter= gensym("jitter");
	
	
	ps_format = gensym("format");
	ps_channels = gensym("channels");
	ps_framesize = gensym("framesize");
	ps_bitrate = gensym("bitrate");
	ps_overflow = gensym("overflow");
	ps_underflow = gensym("underflow");
	ps_queuesize = gensym("queuesize");
	ps_average = gensym("average");
	ps_blocksize = gensym("blocksize");
	ps_hostname = gensym("ipaddr");
	ps_sf_float = gensym("_float_");
	ps_sf_16bit = gensym("_16bit_");
	ps_sf_8bit = gensym("_8bit_");
	ps_sf_mp3 = gensym("_mp3_");
	ps_sf_unknown = gensym("_unknown_");
	ps_nothing = gensym("");
	
}

#else

void nsreceive_tilde_assist(t_nsreceive_tilde *x, void *b, long m, long a, char *s)
{
	switch (m)
	{
		case 1: /* inlet */
			sprintf(s, "(Anything) Control Messages");
			break;
		case 2: /* outlets */
			sprintf(s, "(Signal) Audio Channel %d", (int)(a + 1));
			break;
		break;
	}

}

void main()
{
#ifdef _WINDOWS
    short version = MAKEWORD(2, 0);
    WSADATA nobby;
#endif	/* _WINDOWS */

	setup((t_messlist **)&nsreceive_tilde_class, (method)nsreceive_tilde_new, (method)nsreceive_tilde_free, 
		  (short)sizeof(t_nsreceive_tilde), 0L, A_DEFLONG, A_DEFLONG, A_DEFLONG, 0);
	addmess((method)nsreceive_tilde_dsp, "dsp", A_CANT, 0);
	addmess((method)nsreceive_tilde_assist, "assist", A_CANT, 0);
	addmess((method)nsreceive_tilde_print, "print", 0);
	addmess((method)nsreceive_tilde_reset, "reset", A_DEFFLOAT, 0);
	addmess((method)nsreceive_tilde_reset, "buffer", A_DEFFLOAT, 0);
	// multicast catching (one source per adress)
	addmess((method)nsreceive_tilde_receivefrom, "connect",  A_DEFSYM, A_DEFFLOAT, 0);
	
	addbang((method)nsreceive_tilde_bang);
	dsp_initclass();
	finder_addclass("System", "nsreceive~");



	ps_localhost = gensym("localhost");


	ps_format = gensym("format");
	ps_channels = gensym("channels");
	ps_framesize = gensym("framesize");
	ps_bitrate = gensym("bitrate");
	ps_overflow = gensym("overflow");
	ps_underflow = gensym("underflow");
	ps_queuesize = gensym("queuesize");
	ps_average = gensym("average");
	ps_hostname = gensym("ipaddr");
	ps_sf_float = gensym("_float_");
	ps_sf_16bit = gensym("_16bit_");
	ps_sf_8bit = gensym("_8bit_");
	ps_sf_mp3 = gensym("_mp3_");
	ps_sf_unknown = gensym("_unknown_");
	ps_nothing = gensym("");

#ifdef _WINDOWS
    if (WSAStartup(version, &nobby)) error("nsreceive~: WSAstartup failed");
#endif	/* _WINDOWS */
}
#endif	/* PD */
