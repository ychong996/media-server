/* 
 * File:   RTPICETransport.cpp
 * Author: Sergio
 * 
 * Created on 8 de enero de 2017, 18:37
 */
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/poll.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <srtp2/srtp.h>
#include <time.h>
#include "log.h"
#include "assertions.h"
#include "tools.h"
#include "codecs.h"
#include "rtp.h"
#include "rtpsession.h"
#include "stunmessage.h"
#include <openssl/ossl_typ.h>
#include "DTLSICETransport.h"




DTLSICETransport::DTLSICETransport(Sender *sender) : dtls(*this), mutex(true)
{
	//Store sender
	this->sender = sender;
	//No active candidate
	active = NULL;
	//SRTP instances
	send = NULL;
	recv = NULL;
	//Transport wide seq num
	transportSeqNum = 0;
	feedbackPacketCount = 1;
	feedbackCycles = 0;
	lastFeedbackPacketExtSeqNum = 0;
	//No ice
	iceLocalUsername = NULL;
	iceLocalPwd = NULL;
	iceRemoteUsername = NULL;
	iceRemotePwd = NULL;
}

/*************************
* ~RTPTransport
* 	Destructor
**************************/
DTLSICETransport::~DTLSICETransport()
{
	//Reset
	Reset();
	
}
int DTLSICETransport::onData(const ICERemoteCandidate* candidate,BYTE* data,DWORD size)
{
	RTPHeader header;
	RTPHeaderExtension extension;
		
	//Block method
	ScopedLock method(mutex);
	
	int len = size;
	
	//Check if it a DTLS packet
	if (DTLSConnection::IsDTLS(data,size))
	{
		//Feed it
		dtls.Write(data,size);

		//Read
		//Buffers are always MTU size
		DWORD len = dtls.Read(data,MTU);

		//Check it
		if (len>0)
			//Send it back
			sender->Send(candidate,data,len);
		//Exit
		return 1;
	}
	
	//Check if it is RTCP
	if (RTCPCompoundPacket::IsRTCP(data,size))
	{

		//Check session
		if (!recv)
			return Error("-DTLSICETransport::onData() | No recvSRTPSession\n");
		//unprotect
		srtp_err_status_t err = srtp_unprotect_rtcp(recv,data,&len);
		//Check error
		if (err!=srtp_err_status_ok)
			return Error("-DTLSICETransport::onData() | Error unprotecting rtcp packet [%d]\n",err);

		//Parse it
		RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(data,len);
	
		//Check packet
		if (!rtcp)
		{
			//Debug
			Debug("-RTPBundleTransport::onData() | RTCP wrong data\n");
			//Dump it
			Dump(data,size);
			//Exit
			return 1;
		}
		
		//Process it
		this->onRTCP(rtcp);
		
		rtcp->Dump();
		//Skip
		return 1;
	}
	
	//Check session
	if (!recv)
		return Error("-DTLSICETransport::onData() | No recvSRTPSession\n");
	//unprotect
	srtp_err_status_t err = srtp_unprotect(recv,data,&len);
	//Check status
	if (err!=srtp_err_status_ok)
		//Error
		return Error("-DTLSICETransport::onData() | Error unprotecting rtp packet [%d]\n",err);
	
	//Parse RTP header
	int ini = header.Parse(data,size);
	
	//On error
	if (!ini)
	{
		//Debug
		Debug("-DTLSICETransport::onData() | Could not parse RTP header\n");
		//Dump it
		Dump(data,size);
		//Exit
		return 1;
	}
	
	//If it has extension
	if (header.extension)
	{
		//Parse extension
		int l = extension.Parse(extMap,data+ini,size-ini);
		//If not parsed
		if (!l)
		{
			///Debug
			Debug("-DTLSICETransport::onData() | Could not parse RTP header extension\n");
			//Dump it
			Dump(data,size);
			//Exit
			return 1;
		}
		//Inc ini
		ini += l;
	}
	
	//Get ssrc
	DWORD ssrc = header.ssrc;
	
	//Get incoming group
	auto it = incoming.find(ssrc);
	
	//If not found
	if (it==incoming.end())
		//error
		return Error("-DTLSICETransport::onData() | Unknown ssrc [%u]\n",ssrc);
	
	//Get group
	RTPIncomingSourceGroup *group = it->second;
	
	//Get source for ssrc
	RTPIncomingSource* source = group->GetSource(ssrc);
	
	//Ensure it has a source
	if (!source)
		//error
		return Error("-DTLSICETransport::onData() | Group does not contain ssrc [%u]\n",ssrc);

	//Get initial codec
	BYTE codec = rtpMap.GetCodecForType(header.payloadType);
	
	//Check codec
	if (codec==RTPMap::NotFound)
		//Exit
		return Error("-DTLSICETransport::onData() | RTP packet type unknown [%d]\n",header.payloadType);
	
	//Debug("-Got RTP on sssrc:%u type:%d codec:%d\n",ssrc,type,codec);
	
	//Create normal packet
	RTPPacket *packet = new RTPPacket(group->type,codec,header,extension);
	
	//Set the payload
	packet->SetPayload(data+ini,len-ini);
	
	//Check if we have a sequence wrap
	if (header.sequenceNumber<0x0FFF && (source->extSeq & 0xFFFF)>0xF000)
		//Increase cycles
		source->cycles++;
	
	//Set cycles
	packet->SetSeqCycles(source->cycles);
	
	//Get ext seq
	DWORD extSeq = packet->GetExtSeqNum();
	
	//If we have a not out of order pacekt
	if (extSeq > source->extSeq || !source->numPackets)
		//Update seq num
		source->extSeq = extSeq;
	
	//Increase stats
	source->numPackets++;
	source->totalPacketsSinceLastSR++;
	source->totalBytes += size;
	source->totalBytesSinceLastSR += size;
	

	//Check if it is the min for this SR
	if (extSeq<source->minExtSeqNumSinceLastSR)
		//Store minimum
		source->minExtSeqNumSinceLastSR = extSeq;
	
	//If it is video
	if (group->type == MediaFrame::Video)
	{
		Debug("<-#%u %u %u %u %u\n",packet->GetSeqNum(),packet->GetSeqCycles(),packet->GetTransportSeqNum(),source->extSeq,source->numPackets);
		
		//GEt last 
		WORD transportSeqNum = packet->GetTransportSeqNum();
		
		//Get source
		DWORD source = outgoing.begin()!=outgoing.end() ? outgoing.begin()->second->media.ssrc : 1;

		//Create rtcp transport wide feedback
		RTCPCompoundPacket rtcp;

		//Add to rtcp
		RTCPRTPFeedback* feedback = RTCPRTPFeedback::Create(RTCPRTPFeedback::TransportWideFeedbackMessage,source,ssrc);

		//Create trnasport field
		RTCPRTPFeedback::TransportWideFeedbackMessageField *field = new RTCPRTPFeedback::TransportWideFeedbackMessageField(feedbackPacketCount++);

		//Check if we have a sequence wrap
		if (transportSeqNum<0x0FFF && (lastFeedbackPacketExtSeqNum & 0xFFFF)>0xF000)
			//Increase cycles
			feedbackCycles++;

		//Get extended value
		DWORD transportExtSeqNum = feedbackCycles<<16 | transportSeqNum;
		
		//if not first
		if (lastFeedbackPacketExtSeqNum)
			//For each lost
			for (DWORD i = lastFeedbackPacketExtSeqNum+1; i<transportExtSeqNum; ++i)
				//Add it
				field->packets.insert(std::make_pair(i,0));
		//Store last
		lastFeedbackPacketExtSeqNum = transportExtSeqNum;
		
		//Add this one
		field->packets.insert(std::make_pair(transportSeqNum,getTime()));

		//And add it
		feedback->AddField(field);

		//Add it
		rtcp.AddRTCPacket(feedback);

		//Send packet
		Send(rtcp);
	}
	
	//If it was an RTX packet
	if (ssrc==group->rtx.ssrc) 
	{
		//Ensure that it is a RTX codec
		if (codec!=VideoCodec::RTX)
			//error
			return  Error("-DTLSICETransport::onData() | No RTX codec on rtx sssrc:%u type:%d codec:%d\n",packet->GetSSRC(),packet->GetPayloadType(),packet->GetCodec());
		
		 //Find codec for apt type
		 codec = aptMap.GetCodecForType(packet->GetPayloadType());
		//Check codec
		 if (codec==RTPMap::NotFound)
			  //Error
			  return Error("-DTLSICETransport::ReadRTP(%s) | RTP RTX packet apt type unknown [%d]\n",MediaFrame::TypeToString(packet->GetMedia()),packet->GetPayloadType());
		/*
		   The format of a retransmission packet is shown below:
		    0                   1                   2                   3
		    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		   |                         RTP Header                            |
		   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		   |            OSN                |                               |
		   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
		   |                  Original RTP Packet Payload                  |
		   |                                                               |
		   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		 */
		 //Get original sequence number
		 WORD osn = get2(packet->GetMediaData(),0);

		 UltraDebug("RTX: %s got   %.d:RTX for #%d ts:%u\n",MediaFrame::TypeToString(packet->GetMedia()),packet->GetPayloadType(),osn,packet->GetTimestamp());
		 
		 //Set original seq num
		 packet->SetSeqNum(osn);
		 //Set original ssrc
		 packet->SetSSRC(group->media.ssrc);
		 //Set cycles
		 packet->SetSeqCycles(group->media.cycles);
		 //Set codec
		 packet->SetCodec(codec);
		 
		 //Skip osn from payload
		 packet->SkipPayload(2);
		 
		 //TODO: We should set also the original type
		  UltraDebug("RTX: %s got   %.d:RTX for #%d ts:%u %u %u\n",MediaFrame::TypeToString(packet->GetMedia()),packet->GetPayloadType(),osn,packet->GetTimestamp(),packet->GetExtSeqNum(),group->media.extSeq);
		 //WTF! Drop RTX packets for the future (UUH)
		 if (packet->GetExtSeqNum()>group->media.extSeq)
		 {
			 //DO NOTHING with it yet
			delete(packet);
			//Error
			return Error("Drop RTX future packet [osn:%u,max:%u]\n",osn,group->media.extSeq>>16);
		 }
		 
	} else if (ssrc==group->fec.ssrc)  {
		Debug("-Flex fec\n");
		//Ensure that it is a FEC codec
		if (codec!=VideoCodec::FLEXFEC)
			//error
			return  Error("-DTLSICETransport::onData() | No FLEXFEC codec on fec sssrc:%u type:%d codec:%d\n",MediaFrame::TypeToString(packet->GetMedia()),packet->GetPayloadType(),packet->GetSSRC());
		//DO NOTHING with it yet
		delete(packet);
		return 1;
	}	

	//Update lost packets
	int lost = group->losts.AddPacket(packet);
	
	//Get total lost in queue
	int total = group->losts.GetTotal();

	//Request NACK if it is media
	if (group->type == MediaFrame::Video && lost && ssrc==group->media.ssrc)
	{
		Debug("-lost %d\n",total);
		
		//Create rtcp sender retpor
		RTCPCompoundPacket rtcp;
		
		//If we don't have outgoing have sources
		if ( outgoing.begin()!=outgoing.end())
		{
			//Create report
			RTCPReport *report = source->CreateReport(getTime());

			//Create sender report for normal stream
			RTCPReceiverReport* rr = new RTCPReceiverReport(1);

			//If got anything
			if (report)
				//Append it
				rr->AddReport(report);

			//Append RR to rtcp
			rtcp.AddRTCPacket(rr);
		} else {
			
			//Create report
			RTCPReport *report = source->CreateReport(getTime());

			//Create sender report
			timeval now;
			//Get now
			gettimeofday(&now, NULL);
			//Create sender report for normal stream
			RTCPSenderReport *sr = outgoing.begin()->second->media.CreateSenderReport(&now);

			//If got anything
			if (report)
				//Append it
				sr->AddReport(report);

			//Append RR to rtcp
			rtcp.AddRTCPacket(sr);
		}
	
		//Get nacks for lost
		std::list<RTCPRTPFeedback::NACKField*> nacks = group->losts.GetNacks();
		
		//Get source
		DWORD source = outgoing.begin()!=outgoing.end() ? outgoing.begin()->second->media.ssrc : 1;

		//Create NACK
		RTCPRTPFeedback *nack = RTCPRTPFeedback::Create(RTCPRTPFeedback::NACK,source,ssrc);

		//Add 
		for (std::list<RTCPRTPFeedback::NACKField*>::iterator it = nacks.begin(); it!=nacks.end(); ++it)
			//Add it
			nack->AddField(*it);

		//Add to packet
		rtcp.AddRTCPacket(nack);

		//Send packet
		Send(rtcp);
		rtcp.Dump();
	}

	//Check listener
	if (group->listener)
	{	
		RTPPacket* ordered;
		//Append it to the packets
		group->packets.Add(packet);
		//FOr each ordered packet
		while(ordered=group->packets.GetOrdered())
			//Call listeners
			group->listener->onRTP(group,ordered);
	} else {
		//Drop it
		delete(packet);
	}
	
	//Done
	return 1;
}

void DTLSICETransport::ReSendPacket(RTPOutgoingSourceGroup *group,int seq)
{
	Debug("-DTLSICETransport::ReSendPacket() | resending [seq:%d,ssrc:%u]\n",seq,group->rtx.ssrc);
	
	//Calculate ext seq number
	//TODO: consider warp
	DWORD ext = ((DWORD)(group->media.cycles)<<16 | seq);

	//Find packet to retransmit
	auto it = group->packets.find(ext);

	//If we still have it
	if (it!=group->packets.end())
	{
		//Get packet
		RTPPacket* packet = it->second;
		
		//Get outgoing source
		RTPOutgoingSource& source = group->rtx;

		//Data
		BYTE data[MTU+SRTP_MAX_TRAILER_LEN] ZEROALIGNEDTO32;
		DWORD size = MTU;
		int len = 0;
		
		//Overrride headers
		RTPHeader		header(packet->GetRTPHeader());
		RTPHeaderExtension	extension(packet->GetRTPHeaderExtension());
		
		//Update RTX headers
		header.ssrc		= source.ssrc;
		header.payloadType	= rtpMap.GetTypeForCodec(VideoCodec::RTX);
		header.sequenceNumber	= source.extSeq++;
		//No padding
		header.padding		= 0;

		//Calculate last timestamp
		source.lastTime = source.time + packet->GetTimestamp();

		//Check seq wrap
		if (source.extSeq==0)
			//Inc cycles
			source.cycles++;

		//Add transport wide cc on video
		if (group->type == MediaFrame::Video)
		{
			//Add extension
			header.extension = true;
			//Add transport
			extension.hasTransportWideCC = true;
			extension.transportSeqNum = transportSeqNum++;
		}
		
		//Serialize header
		int n = header.Serialize(data,size);

		//Comprobamos que quepan
		if (!n)
			//Error
			return (void)Error("-DTLSICETransport::ReSendPacket() | Error serializing rtp headers\n");
		
		//Inc len
		len += n;
		
		//If we have extension
		if (header.extension)
		{
			//Serialize
			n = extension.Serialize(extMap,data+len,size-len);
			//Comprobamos que quepan
			if (!n)
				//Error
				return (void)Error("-DTLSICETransport::ReSendPacket() | Error serializing rtp extension headers\n");
			//Inc len
			len += n;
		}
		
		//And set the original seq
		set2(data,len,seq);
		//Move payload start
		len += 2;
		
		//Ensure we have enougth data
		if (len+packet->GetMediaLength()>size)
			//Error
			return (void)Error("-DTLSICETransport::ReSendPacket() | Media overflow\n");

		//Copiamos los datos
		memcpy(data+len,packet->GetMediaData(),packet->GetMediaLength());

		header.Dump();
		extension.Dump();
		Dump(data+len-2,16);
		
		//Set pateckt length
		len += packet->GetMediaLength();
		
		//Encript
		srtp_err_status_t err = srtp_protect(send,data,&len);
		//Check error
		if (err!=srtp_err_status_ok)
			//Error
			return (void)Error("-RTPTransport::SendPacket() | Error protecting RTP packet [%d]\n",err);

		
		//No error yet, send packet
		len = sender->Send(active,data,len);
		
		if (len<=0)
			Error("-RTPTransport::SendPacket() | Error sending RTP packet [%d]\n",errno);
		
	} else {
		Debug("-DTLSICETransport::ReSendPacket() | Packet not found  not found [seq:%u]\n",seq);
	}
}


ICERemoteCandidate* DTLSICETransport::AddRemoteCandidate(const sockaddr_in addr, bool useCandidate, DWORD priority)
{
	//Block method
	ScopedLock method(mutex);
	
	//Debug
	Debug("-DTLSICETransport::AddRemoteCandidate() | Remote candidate [%s:%d,use:%d,prio:%d]\n",inet_ntoa(addr.sin_addr),ntohs(addr.sin_port),useCandidate,priority);
	
	//Create new candidate
	ICERemoteCandidate* candidate = new ICERemoteCandidate(inet_ntoa(addr.sin_addr),ntohs(addr.sin_port),this);
	
	//Add to candidates
	candidates.push_back(candidate);
	
	//Should we set this candidate as the active one
	if (!active  || useCandidate)
		//Send data to this one from now on
		active = candidate;
	
	BYTE data[MTU+SRTP_MAX_TRAILER_LEN] ZEROALIGNEDTO32;
	
	// Needed for DTLS in client mode (otherwise the DTLS "Client Hello" is not sent over the wire)
	DWORD len = dtls.Read(data,MTU);
	//Check it
	if (len>0)
		//Send to bundle transport
		sender->Send(active,data,len);
	
	//Return it
	return candidate;
}

void DTLSICETransport::SetProperties(const Properties& properties)
{
	//Cleant maps
	rtpMap.clear();
	extMap.clear();
	
	//Get audio codecs
	Properties audio;
	properties.GetChildren("audio",audio);

	//TODO: support all
	rtpMap[audio.GetProperty("opus.pt",0)] = AudioCodec::OPUS;
	
	//Get video codecs
	Properties video;
	properties.GetChildren("video",video);
	
	//TODO: support all
	rtpMap[video.GetProperty("vp9.pt",0)] = VideoCodec::VP9;
	rtpMap[video.GetProperty("vp9.rtx",0)] = VideoCodec::RTX;
	rtpMap[video.GetProperty("flexfec.pt",0)] = VideoCodec::FLEXFEC;
	
	//Add apt
	aptMap[video.GetProperty("vp9.rtx",0)] = VideoCodec::VP9;
	
	//Get extensions headers
	Properties headers;
	properties.GetChildren("ext",headers);
	
	//For each extension
	for (Properties::const_iterator it=headers.begin();it!=headers.end();++it)
	{
		//Set extension
		if (it->first.compare("urn:ietf:params:rtp-hdrext:toffset")==0) {
			//Set extension
			extMap[atoi(it->second.c_str())] = RTPHeaderExtension::TimeOffset;
		} else if (it->first.compare("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time")==0) {
			//Set extension
			extMap[atoi(it->second.c_str())] = RTPHeaderExtension::AbsoluteSendTime;
		} else if (it->first.compare("urn:ietf:params:rtp-hdrext:ssrc-audio-level")==0) {
			//Set extension
			extMap[atoi(it->second.c_str())] = RTPHeaderExtension::SSRCAudioLevel;
		} else if (it->first.compare("urn:3gpp:video-orientation")==0) {
			//Set extension
			extMap[atoi(it->second.c_str())] = RTPHeaderExtension::CoordinationOfVideoOrientation;
		} else if (it->first.compare("http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01")==0) {
			//Set extension
			extMap[atoi(it->second.c_str())] = RTPHeaderExtension::TransportWideCC;
		} else {
			Error("-DTLSICETransport::SetProperties() | Unknown RTP property [%s]\n",it->first.c_str());
		}
	}
}
			
void DTLSICETransport::Reset()
{
	Log("-RTPBundleTransport reset\n");

	//Clean mem
	if (iceLocalUsername)
		free(iceLocalUsername);
	if (iceLocalPwd)
		free(iceLocalPwd);
	if (iceRemoteUsername)
		free(iceRemoteUsername);
	if (iceRemotePwd)
		free(iceRemotePwd);
	//If secure
	if (send)
		//Dealoacate
		srtp_dealloc(send);
	//If secure
	if (recv)
		//Dealoacate
		srtp_dealloc(recv);
	
	send = NULL;
	recv = NULL;
	//No ice
	iceLocalUsername = NULL;
	iceLocalPwd = NULL;
	iceRemoteUsername = NULL;
	iceRemotePwd = NULL;
}

int DTLSICETransport::SetLocalCryptoSDES(const char* suite,const BYTE* key,const DWORD len)
{
	srtp_err_status_t err;
	srtp_policy_t policy;

	//empty policy
	memset(&policy, 0, sizeof(srtp_policy_t));

	//Get cypher
	if (strcmp(suite,"AES_CM_128_HMAC_SHA1_80")==0)
	{
		Log("-RTPBundleTransport::SetLocalCryptoSDES() | suite: AES_CM_128_HMAC_SHA1_80\n");
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	} else if (strcmp(suite,"AES_CM_128_HMAC_SHA1_32")==0) {
		Log("-RTPBundleTransport::SetLocalCryptoSDES() | suite: AES_CM_128_HMAC_SHA1_32\n");
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);  // NOTE: Must be 80 for RTCP!
	} else if (strcmp(suite,"AES_CM_128_NULL_AUTH")==0) {
		Log("-RTPBundleTransport::SetLocalCryptoSDES() | suite: AES_CM_128_NULL_AUTH\n");
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtcp);
	} else if (strcmp(suite,"NULL_CIPHER_HMAC_SHA1_80")==0) {
		Log("-RTPBundleTransport::SetLocalCryptoSDES() | suite: NULL_CIPHER_HMAC_SHA1_80\n");
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtcp);
	} else {
		return Error("-RTPBundleTransport::SetLocalCryptoSDES() | Unknown cipher suite: %s", suite);
	}

	//Check sizes
	if (len!=policy.rtp.cipher_key_len)
		//Error
		return Error("-RTPBundleTransport::SetLocalCryptoSDES() | Key size (%d) doesn't match the selected srtp profile (required %d)\n",len,policy.rtp.cipher_key_len);

	//Set polciy values
	policy.ssrc.type	= ssrc_any_outbound;
	policy.ssrc.value	= 0;
	policy.allow_repeat_tx  = 1;
	policy.window_size	= 1024;
	policy.key		= (BYTE*)key;
	policy.next		= NULL;
	//Create new
	srtp_t session;
	err = srtp_create(&session,&policy);

	//Check error
	if (err!=srtp_err_status_ok)
		//Error
		return Error("-RTPBundleTransport::SetLocalCryptoSDES() | Failed to create local SRTP session | err:%d\n", err);
	
	//if we already got a send session don't leak it
	if (send)
		//Dealoacate
		srtp_dealloc(send);

	//Set send SSRTP sesion
	send = session;

	//Evrything ok
	return 1;
}


int DTLSICETransport::SetLocalSTUNCredentials(const char* username, const char* pwd)
{
	Log("-RTPBundleTransport::SetLocalSTUNCredentials() | [frag:%s,pwd:%s]\n",username,pwd);
	//Clean mem
	if (iceLocalUsername)
		free(iceLocalUsername);
	if (iceLocalPwd)
		free(iceLocalPwd);
	//Store values
	iceLocalUsername = strdup(username);
	iceLocalPwd = strdup(pwd);
	//Ok
	return 1;
}


int DTLSICETransport::SetRemoteSTUNCredentials(const char* username, const char* pwd)
{
	Log("-RTPBundleTransport::SetRemoteSTUNCredentials() |  [frag:%s,pwd:%s]\n",username,pwd);
	//Clean mem
	if (iceRemoteUsername)
		free(iceRemoteUsername);
	if (iceRemotePwd)
		free(iceRemotePwd);
	//Store values
	iceRemoteUsername = strdup(username);
	iceRemotePwd = strdup(pwd);
	//Ok
	return 1;
}

int DTLSICETransport::SetRemoteCryptoDTLS(const char *setup,const char *hash,const char *fingerprint)
{
	Log("-RTPBundleTransport::SetRemoteCryptoDTLS | [setup:%s,hash:%s,fingerprint:%s]\n",setup,hash,fingerprint);

	//Set Suite
	if (strcasecmp(setup,"active")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_ACTIVE);
	else if (strcasecmp(setup,"passive")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_PASSIVE);
	else if (strcasecmp(setup,"actpass")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_ACTPASS);
	else if (strcasecmp(setup,"holdconn")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_HOLDCONN);
	else
		return Error("-RTPBundleTransport::SetRemoteCryptoDTLS | Unknown setup");

	//Set fingerprint
	if (strcasecmp(hash,"SHA-1")==0)
		dtls.SetRemoteFingerprint(DTLSConnection::SHA1,fingerprint);
	else if (strcasecmp(hash,"SHA-224")==0)
		dtls.SetRemoteFingerprint(DTLSConnection::SHA224,fingerprint);
	else if (strcasecmp(hash,"SHA-256")==0)
		dtls.SetRemoteFingerprint(DTLSConnection::SHA256,fingerprint);
	else if (strcasecmp(hash,"SHA-384")==0)
		dtls.SetRemoteFingerprint(DTLSConnection::SHA384,fingerprint);
	else if (strcasecmp(hash,"SHA-512")==0)
		dtls.SetRemoteFingerprint(DTLSConnection::SHA512,fingerprint);
	else
		return Error("-RTPBundleTransport::SetRemoteCryptoDTLS | Unknown hash");

	//Init DTLS
	return dtls.Init();
}

int DTLSICETransport::SetRemoteCryptoSDES(const char* suite, const BYTE* key, const DWORD len)
{
	srtp_err_status_t err;
	srtp_policy_t policy;

	//empty policy
	memset(&policy, 0, sizeof(srtp_policy_t));

	if (strcmp(suite,"AES_CM_128_HMAC_SHA1_80")==0)
	{
		Log("-RTPBundleTransport::SetRemoteCryptoSDES() | suite: AES_CM_128_HMAC_SHA1_80\n");
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	} else if (strcmp(suite,"AES_CM_128_HMAC_SHA1_32")==0) {
		Log("-RTPBundleTransport::SetRemoteCryptoSDES() | suite: AES_CM_128_HMAC_SHA1_32\n");
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);  // NOTE: Must be 80 for RTCP!
	} else if (strcmp(suite,"AES_CM_128_NULL_AUTH")==0) {
		Log("-RTPBundleTransport::SetRemoteCryptoSDES() | suite: AES_CM_128_NULL_AUTH\n");
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtcp);
	} else if (strcmp(suite,"NULL_CIPHER_HMAC_SHA1_80")==0) {
		Log("-RTPBundleTransport::SetRemoteCryptoSDES() | suite: NULL_CIPHER_HMAC_SHA1_80\n");
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtcp);
	} else {
		return Error("-RTPBundleTransport::SetRemoteCryptoSDES() | Unknown cipher suite %s", suite);
	}

	//Check sizes
	if (len!=policy.rtp.cipher_key_len)
		//Error
		return Error("-RTPBundleTransport::SetRemoteCryptoSDES() | Key size (%d) doesn't match the selected srtp profile (required %d)\n",len,policy.rtp.cipher_key_len);

	//Set polciy values
	policy.ssrc.type	= ssrc_any_inbound;
	policy.ssrc.value	= 0;
	policy.key		= (BYTE*)key;
	policy.next		= NULL;

	//Create new
	srtp_t session;
	err = srtp_create(&session,&policy);

	//Check error
	if (err!=srtp_err_status_ok)
		//Error
		return Error("-RTPBundleTransport::SetRemoteCryptoSDES() | Failed to create remote SRTP session | err:%d\n", err);
	
	//if we already got a recv session don't leak it
	if (recv)
		//Dealoacate
		srtp_dealloc(recv);
	//Set it
	recv = session;

	//Everything ok
	return 1;
}

void DTLSICETransport::onDTLSSetup(DTLSConnection::Suite suite,BYTE* localMasterKey,DWORD localMasterKeySize,BYTE* remoteMasterKey,DWORD remoteMasterKeySize)
{
	Log("-RTPBundleTransport::onDTLSSetup()\n");

	switch (suite)
	{
		case DTLSConnection::AES_CM_128_HMAC_SHA1_80:
			//Set keys
			SetLocalCryptoSDES("AES_CM_128_HMAC_SHA1_80",localMasterKey,localMasterKeySize);
			SetRemoteCryptoSDES("AES_CM_128_HMAC_SHA1_80",remoteMasterKey,remoteMasterKeySize);
			break;
		case DTLSConnection::AES_CM_128_HMAC_SHA1_32:
			//Set keys
			SetLocalCryptoSDES("AES_CM_128_HMAC_SHA1_32",localMasterKey,localMasterKeySize);
			SetRemoteCryptoSDES("AES_CM_128_HMAC_SHA1_32",remoteMasterKey,remoteMasterKeySize);
			break;
		case DTLSConnection::F8_128_HMAC_SHA1_80:
			//Set keys
			SetLocalCryptoSDES("NULL_CIPHER_HMAC_SHA1_80",localMasterKey,localMasterKeySize);
			SetRemoteCryptoSDES("NULL_CIPHER_HMAC_SHA1_80",remoteMasterKey,remoteMasterKeySize);
			break;
	}
}

bool DTLSICETransport::AddOutgoingSourceGroup(RTPOutgoingSourceGroup *group)
{
	//Add it for each group ssrc
	outgoing[group->media.ssrc] = group;
	if (group->fec.ssrc)
		outgoing[group->fec.ssrc] = group;
	if (group->rtx.ssrc)
		outgoing[group->rtx.ssrc] = group;
	
	return true;
	
}

bool DTLSICETransport::RemoveOutgoingSourceGroup(RTPOutgoingSourceGroup *group)
{
	//Remove it from each ssrc
	outgoing.erase(group->media.ssrc);
	if (group->fec.ssrc)
		outgoing.erase(group->fec.ssrc);
	if (group->rtx.ssrc)
		outgoing.erase(group->rtx.ssrc);
	
	return true;
}

bool DTLSICETransport::AddIncomingSourceGroup(RTPIncomingSourceGroup *group)
{
	Log("-AddIncomingSourceGroup [ssrc:%u,fec:%u,rtx:%u]\n",group->media.ssrc,group->fec.ssrc,group->rtx.ssrc);
	
	//It must contain media ssrc
	if (!group->media.ssrc)
		return Error("No media ssrc defining, stream will not be added\n");
		
	//Add it for each group ssrc	
	incoming[group->media.ssrc] = group;
	if (group->fec.ssrc)
		incoming[group->fec.ssrc] = group;
	if (group->rtx.ssrc)
		incoming[group->rtx.ssrc] = group;
	//Done
	return true;
}

bool DTLSICETransport::RemoveIncomingSourceGroup(RTPIncomingSourceGroup *group)
{
	Log("-RemoveIncomingSourceGroup [ssrc:%u,fec:%u,rtx:%u]\n",group->media.ssrc,group->fec.ssrc,group->rtx.ssrc);
	
	//It must contain media ssrc
	if (!group->media.ssrc)
		return Error("No media ssrc defined, stream will not be removed\n");
	
	//Remove it from each ssrc
	incoming.erase(group->media.ssrc);
	if (group->fec.ssrc)
		incoming.erase(group->fec.ssrc);
	if (group->rtx.ssrc)
		incoming.erase(group->rtx.ssrc);
	
	//Done
	return true;
}
void DTLSICETransport::Send(RTCPCompoundPacket &rtcp)
{
	BYTE 	data[MTU+SRTP_MAX_TRAILER_LEN] ALIGNEDTO32;
	DWORD   size = MTU;
	
	//If we don't have an active candidate yet
	if (!active)
		//Error
		return (void) Debug("-DTLSICETransport::Send() | We don't have an active candidate yet\n");
	if (!send)
		//Error
		return (void) Debug("-DTLSICETransport::Send() | We don't have an DTLS setup yet\n");
	
	//Serialize
	int len = rtcp.Serialize(data,size);
	
	//Check result
	if (len<=0 || len>size)
		//Error
		return (void)Error("-DTLSICETransport::Send() | Error serializing RTCP packet [len:%d]\n",len);

	
	//Encript
	srtp_err_status_t srtp_err_status = srtp_protect_rtcp(send,data,&len);
	//Check error
	if (srtp_err_status!=srtp_err_status_ok)
		//Error
		return (void)Error("-DTLSICETransport::Send() | Error protecting RTCP packet [%d]\n",srtp_err_status);

	//No error yet, send packet
	int ret = sender->Send(active,data,len);
	
	if (ret<=0)
		Debug("-Error sending RTCP packet [ret:%d,%d]\n",ret,errno);
}

void DTLSICETransport::SendPLI(DWORD ssrc)
{
	//Block method
	ScopedLock method(mutex);
	
	//Create rtcp sender retpor
	RTCPCompoundPacket rtcp;

	DWORD source = outgoing.begin()!=outgoing.end() ? outgoing.begin()->second->media.ssrc : 1;
	
	//Find incoming source
	auto it = incoming.find(ssrc);
	
	//If not found
	if (it==incoming.end())
		return (void)Error("- DTLSICETransport::SendPLI() | no incoming source found for [ssrc:%u]\n",ssrc);
	
	//Drop all pending and lost packets
	it->second->packets.Reset();
	it->second->losts.Reset();
	
	//Add to rtcp
	rtcp.AddRTCPacket( RTCPPayloadFeedback::Create(RTCPPayloadFeedback::PictureLossIndication,source,ssrc));

	//Send packet
	Send(rtcp);
}

void DTLSICETransport::Send(RTPPacket &packet)
{
	BYTE 	data[MTU+SRTP_MAX_TRAILER_LEN] ALIGNEDTO32;
	DWORD	size = MTU;
	
	//Block method
	ScopedLock method(mutex);
	
	//If we don't have an active candidate yet
	if (!active)
		//Error
		return (void) Debug("-DTLSICETransport::Send() | We don't have an active candidate yet\n");
	if (!send)
		//Error
		return (void) Debug("-DTLSICETransport::Send() | We don't have an DTLS setup yet\n");
	
	//Find outgoing source
	auto it = outgoing.find(packet.GetSSRC());
	
	//If not found
	if (it==outgoing.end())
		//Error
		return (void) Error("-DTLSICETransport::Send() | Outgoind source not registered for ssrc:%u\n",packet.GetSSRC());
	
	//Get outgoing group
	RTPOutgoingSourceGroup* group = it->second;
	//Get outgoing source
	RTPOutgoingSource& source = group->media;
	
	//Overrride headers
	RTPHeader		header(packet.GetRTPHeader());
	RTPHeaderExtension	extension(packet.GetRTPHeaderExtension());

	//Update RTX headers
	header.ssrc		= source.ssrc;
	header.payloadType	= rtpMap.GetTypeForCodec(packet.GetCodec());
	//No padding
	header.padding		= 0;

	//Calculate last timestamp
	source.lastTime = source.time + packet.GetTimestamp();
	source.extSeq = packet.GetExtSeqNum();

	//Check seq wrap
	if (source.extSeq==0)
		//Inc cycles
		source.cycles++;

	//Add transport wide cc on video
	if (group->type == MediaFrame::Video)
	{
		//Add extension
		header.extension = true;
		//Add transport
		extension.hasTransportWideCC = true;
		extension.transportSeqNum = transportSeqNum++;
	}

	//Serialize header
	int len = header.Serialize(data,size);

	//Check
	if (!len)
		//Error
		return (void)Error("-DTLSICETransport::SendPacket() | Error serializing rtp headers\n");

	//If we have extension
	if (header.extension)
	{
		//Serialize
		int n = extension.Serialize(extMap,data+len,size-len);
		//Comprobamos que quepan
		if (!n)
			//Error
			return (void)Error("-DTLSICETransport::SendPacket() | Error serializing rtp extension headers\n");
		//Inc len
		len += n;
	}

	//Ensure we have enougth data
	if (len+packet.GetMediaLength()>size)
		//Error
			return (void)Error("-DTLSICETransport::SendPacket() | Media overflow\n");

	//Copiamos los datos
	memcpy(data+len,packet.GetMediaData(),packet.GetMediaLength());

	//Set pateckt length
	len += packet.GetMediaLength();

	//If there is an rtx stream
	if (group->rtx.ssrc)
	{
		//Create new packet with this data
		RTPPacket *rtx = new RTPPacket(packet.GetMedia(),packet.GetCodec(),header,extension);
		//Set media
		rtx->SetPayload(packet.GetMediaData(),packet.GetMediaLength());
		//Add a clone to the rtx queue
		group->packets[packet.GetExtSeqNum()] = rtx;
	}
	
	
	//Encript
	srtp_err_status_t err = srtp_protect(send,data,&len);
	//Check error
	if (err!=srtp_err_status_ok)
		//Error
		return (void)Error("-RTPTransport::SendPacket() | Error protecting RTP packet [%d]\n",err);
	
	//If it is video
	if (group->type == MediaFrame::Video)
		Debug("->#%u %u %u %u rtx:%u\n",packet.GetSeqNum(),source.cycles,packet.GetExtSeqNum(),source.extSeq,group->rtx.ssrc);

	//if (group->type == MediaFrame::Video && source.extSeq%16!=0)
		//No error yet, send packet
		len = sender->Send(active,data,len);
	/*else
	{
		Debug("-----droping %u len:%d\n",source.extSeq,len);
		packet.Dump();
	}*/
	
	//If got packet to send
	if (len>0)
	{
		//Inc stats
		source.numPackets++;
		source.totalBytes += len;
	} else {
		Error("-ERROR SENDING packet\n");
	}

	//Get time for packets to discard, always have at least 200ms, max 500ms
	DWORD rtt = 0;
	QWORD until = getTime()/1000 - (200+fmin(rtt*2,300));
	//Delete old packets
	auto it2 = group->packets.begin();
	//Until the end
	while(it2!=group->packets.end())
	{
		//Get pacekt
		RTPPacket *pkt = it2->second;
		//Check time
		if (pkt->GetTime()>until)
			//Keep the rest
			break;
		//DElete from queue and move next
		group->packets.erase(it2++);
		//Delete object
		delete(pkt);
	}
}


void DTLSICETransport::onRTCP(RTCPCompoundPacket* rtcp)
{
	//For each packet
	for (int i = 0; i<rtcp->GetPacketCount();i++)
	{
		//Get pacekt
		const RTCPPacket* packet = rtcp->GetPacket(i);
		//Check packet type
		switch (packet->GetType())
		{
			case RTCPPacket::SenderReport:
			{
				const RTCPSenderReport* sr = (const RTCPSenderReport*)packet;
				
				//Get ssrc
				DWORD ssrc = sr->GetSSRC();
				//Get the incouming source
				auto it = incoming.find(ssrc);
				
				//If not found
				if (it==incoming.end())
				{
					Error("-Could not find incoming source for RTCP SR [ssrc:%u]\n",ssrc);
					rtcp->Dump();
					continue;
				}
				//Get source froup
				RTPIncomingSourceGroup *group = it->second;
				
				//Get source
				RTPIncomingSource* source = group->GetSource(ssrc);
				
				//Store info
				source->lastReceivedSenderNTPTimestamp = sr->GetNTPTimestamp();
				source->lastReceivedSenderReport = getTime();
				
				for (int j=0;j<sr->GetCount();j++)
				{
					//Get report
					RTCPReport *report = sr->GetReport(j);
					//Check ssrc
					DWORD ssrc = report->GetSSRC();
					//TODO: Do something
				}
				break;
			}
			case RTCPPacket::ReceiverReport:
			{
				const RTCPReceiverReport* rr = (const RTCPReceiverReport*)packet;
				//Check recievd report
				for (int j=0;j<rr->GetCount();j++)
				{
					//Get report
					RTCPReport *report = rr->GetReport(j);
					//Check ssrc
					DWORD ssrc = report->GetSSRC();
				}
				break;
			}
			case RTCPPacket::SDES:
				break;
			case RTCPPacket::Bye:
				break;
			case RTCPPacket::App:
				break;
			case RTCPPacket::RTPFeedback:
			{
				
				//Get feedback packet
				RTCPRTPFeedback *fb = (RTCPRTPFeedback*) packet;
				//Get SSRC for media
				DWORD ssrc = fb->GetMediaSSRC();
				//Find ouggoing source
				auto it = outgoing.find(ssrc);
				//If not found
				if (it == outgoing.end())
				{
					//Dump
					fb->Dump();
					//Debug
					Error("-DTLSICETransport::onRTCP() | Got feedback message for unknown media  [ssrc:%u]\n",ssrc);
					//Ups! Skip
					continue;
				}
				//Get media
				RTPOutgoingSourceGroup* group = it->second;
				//Check feedback type
				switch(fb->GetFeedbackType())
				{
					case RTCPRTPFeedback::NACK:
						fb->Dump();
						for (BYTE i=0;i<fb->GetFieldCount();i++)
						{
							//Get field
							const RTCPRTPFeedback::NACKField *field = (const RTCPRTPFeedback::NACKField*) fb->GetField(i);
							
							//Resent it
							ReSendPacket(group,field->pid);
							//Check each bit of the mask
							for (int i=0;i<16;i++)
								//Check it bit is present to rtx the packets
								if ((field->blp >> (15-i)) & 1)
									//Resent it
									ReSendPacket(group,field->pid+i+1);
						}
						break;
					case RTCPRTPFeedback::TempMaxMediaStreamBitrateRequest:
						Debug("-DTLSICETransport::onRTCP() | TempMaxMediaStreamBitrateRequest\n");
						break;
					case RTCPRTPFeedback::TempMaxMediaStreamBitrateNotification:
						Debug("-DTLSICETransport::onRTCP() | TempMaxMediaStreamBitrateNotification\n");
						break;
					case RTCPRTPFeedback::TransportWideFeedbackMessage:
						Debug("-DTLSICETransport::onRTCP() | TransportWideFeedbackMessage\n");
						break;
				}
				break;
			}
			case RTCPPacket::PayloadFeedback:
			{
				//Get feedback packet
				RTCPPayloadFeedback *fb = (RTCPPayloadFeedback*) packet;
				//Get SSRC for media
				DWORD ssrc = fb->GetMediaSSRC();
				//Find ouggoing source
				auto it = outgoing.find(ssrc);
				//If not found
				if (it == outgoing.end())
				{
					//Dump
					fb->Dump();
					//Debug
					Error("-Got feedback message for unknown media  [ssrc:%u]\n",ssrc);
					//Ups! Skip
					continue;
				}
				//Get media
				RTPOutgoingSourceGroup* group = it->second;
				//Check feedback type
				switch(fb->GetFeedbackType())
				{
					case RTCPPayloadFeedback::PictureLossIndication:
					case RTCPPayloadFeedback::FullIntraRequest:
						Debug("-DTLSICETransport::onRTCP() | FPU requested [ssrc:%u]\n",ssrc);
						//Check listener
						if (group->listener)
							//Call listeners
							group->listener->onPLIRequest(group,ssrc);
						//Get media
					case RTCPPayloadFeedback::SliceLossIndication:
						Debug("-DTLSICETransport::onRTCP() | SliceLossIndication\n");
						break;
					case RTCPPayloadFeedback::ReferencePictureSelectionIndication:
						Debug("-DTLSICETransport::onRTCP() | ReferencePictureSelectionIndication\n");
						break;
					case RTCPPayloadFeedback::TemporalSpatialTradeOffRequest:
						Debug("-DTLSICETransport::onRTCP() | TemporalSpatialTradeOffRequest\n");
						break;
					case RTCPPayloadFeedback::TemporalSpatialTradeOffNotification:
						Debug("-DTLSICETransport::onRTCP() | TemporalSpatialTradeOffNotification\n");
						break;
					case RTCPPayloadFeedback::VideoBackChannelMessage:
						Debug("-DTLSICETransport::onRTCP() | VideoBackChannelMessage\n");
						break;
					case RTCPPayloadFeedback::ApplicationLayerFeeedbackMessage:
						for (BYTE i=0;i<fb->GetFieldCount();i++)
						{
							//Get feedback
							const RTCPPayloadFeedback::ApplicationLayerFeeedbackField* msg = (const RTCPPayloadFeedback::ApplicationLayerFeeedbackField*)fb->GetField(i);
							//Get size and payload
							DWORD len		= msg->GetLength();
							const BYTE* payload	= msg->GetPayload();
							//Check if it is a REMB
							if (len>8 && payload[0]=='R' && payload[1]=='E' && payload[2]=='M' && payload[3]=='B')
							{
								//GEt exponent
								BYTE exp = payload[5] >> 2;
								DWORD mantisa = payload[5] & 0x03;
								mantisa = mantisa << 8 | payload[6];
								mantisa = mantisa << 8 | payload[7];
								//Get bitrate
								DWORD bitrate = mantisa << exp;
							}
						}
						break;
				}
				break;
			}
			case RTCPPacket::FullIntraRequest:
				//THis is deprecated
				Debug("-DTLSICETransport::onRTCP() | FullIntraRequest!\n");
				break;
			case RTCPPacket::NACK:
				//THis is deprecated
				Debug("-DTLSICETransport::onRTCP() | NACK!\n");
				break;
		}
	}
}