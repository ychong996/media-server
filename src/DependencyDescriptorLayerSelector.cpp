#include <vector>

#include "DependencyDescriptorLayerSelector.h"

constexpr uint32_t NoChain		= std::numeric_limits<uint32_t>::max();
constexpr uint32_t NoDecodeTarget	= std::numeric_limits<uint32_t>::max();
constexpr uint64_t NoFrame		= std::numeric_limits<uint64_t>::max();
	
DependencyDescriptorLayerSelector::DependencyDescriptorLayerSelector(VideoCodec::Type codec)
{
	this->codec = codec;
}

void DependencyDescriptorLayerSelector::SelectTemporalLayer(BYTE id)
{
	//Set next
	temporalLayerId = id;
}

void DependencyDescriptorLayerSelector::SelectSpatialLayer(BYTE id)
{
	//Set next
	spatialLayerId = id;
}
	
bool DependencyDescriptorLayerSelector::Select(const RTPPacket::shared& packet,bool &mark)
{
	//Get dependency description
	auto& dependencyDescriptor = packet->GetDependencyDescriptor();
	auto& currentTemplateDependencyStructure = packet->GetTemplateDependencyStructure();
	auto& activeDecodeTargets = packet->GetActiveDecodeTargets();
	
	//Check rtp packet has a frame descriptor
	if (!dependencyDescriptor)
	{
		//Request intra
		waitingForIntra = true;
		//Error
		return Warning("-DependencyDescriptorLayerSelector::Select() | coulnd't retrieve DependencyDestriptor\n");
	}
	
	//Check we already have received a template structure for this rtp stream
	if (!currentTemplateDependencyStructure)
	{
		//Request intra
		waitingForIntra = true;
		//Error
		return Warning("-DependencyDescriptorLayerSelector::Select() | coulnd't retrieve current TemplateDependencyStructure\n");
	}
	
	//Get extended frame number
	auto extFrameNum = frameNumberExtender.Extend(dependencyDescriptor->frameNumber);
	
	//Check if we have not received the first frame 
	if (currentFrameNumber==std::numeric_limits<uint32_t>::max())
	{
		//If it is not first packet in frame
		if (!dependencyDescriptor->startOfFrame)
		{
			//Request intra
			waitingForIntra = true;
			//Ignore packet
			return false;
		}
		
		//Store current frame
		currentFrameNumber = extFrameNum;
	}
	
	//Ensure that we have the packet frame dependency template
	if (!currentTemplateDependencyStructure->ContainsFrameDependencyTemplate(dependencyDescriptor->frameDependencyTemplateId))
		//Skip
		return Warning("-DependencyDescriptorLayerSelector::Select() | Current frame dependency templates don't contain reference templateId [id:%d]\n",dependencyDescriptor->frameDependencyTemplateId);
	
	//Get template
	const auto& frameDependencyTemplate = currentTemplateDependencyStructure->GetFrameDependencyTemplate(dependencyDescriptor->frameDependencyTemplateId);
	
	//Get dtis for current frame
	auto& decodeTargetIndications	= dependencyDescriptor->customDecodeTargetIndications	? dependencyDescriptor->customDecodeTargetIndications.value()	: frameDependencyTemplate.decodeTargetIndications; 
	auto& frameDiffs		= dependencyDescriptor->customFrameDiffs		? dependencyDescriptor->customFrameDiffs.value()		: frameDependencyTemplate.frameDiffs;
	auto& frameDiffsChains		= dependencyDescriptor->customFrameDiffsChains		? dependencyDescriptor->customFrameDiffsChains.value()		: frameDependencyTemplate.frameDiffsChains;
	
	//Check if it is decodable
	bool decodable = true;
	
	//We will only forward full frames
	// TODO: check rtp seq num continuity?
	if (extFrameNum>currentFrameNumber && !dependencyDescriptor->startOfFrame)
		//Discard frame
		decodable = false;
	
	//Get all referenced frames
	for(size_t i=0; i<frameDiffs.size() && decodable; ++i)
	{
		//Calculate frame number from diff
		auto referencedFrame = extFrameNum - frameDiffs[i];
		//If it is not us
		if (referencedFrame!=currentFrameNumber)
			//Check if we have sent it
			decodable = forwardedFrames.Contains(referencedFrame);
	}
	
	//No chain or decode target
	auto currentChain	 = NoChain;
	auto currentDecodeTarget = NoDecodeTarget;
	
	//Seach best lahyer target for this spatial and temporal layer
	for (uint32_t i = 0; i<currentTemplateDependencyStructure->dtisCount; ++i)
	{
		//Iterate in reverse order, high spatial layers first, then temporal layers within same spatial layer
		uint32_t decodeTarget = currentTemplateDependencyStructure->dtisCount-i-1;
		//Check if it is our current selected layer 
		if (currentTemplateDependencyStructure->decodeTargetLayerMapping[decodeTarget].spatialLayerId <= spatialLayerId && 
		    currentTemplateDependencyStructure->decodeTargetLayerMapping[decodeTarget].temporalLayerId <= temporalLayerId )
		{
			//If decode target is active
			if (activeDecodeTargets && activeDecodeTargets->at(decodeTarget))
			{
				//Check we have chain for current target
				if (decodeTarget < currentTemplateDependencyStructure->decodeTargetProtectedByChain.size())
				{
					//Get chain for current target
					auto chain = currentTemplateDependencyStructure->decodeTargetProtectedByChain[decodeTarget];
					
					//Check dti info is correct
					if (decodeTargetIndications.size()<decodeTarget)
						//Try next
						continue;

					//If frame is not present in current target
					if (decodeTargetIndications[decodeTarget]==DecodeTargetIndication::NotPresent)
						//Try next
						continue;
					
					//Check chain info is correct
					if (frameDiffsChains.size()<chain)
						//Try next
						continue;
					
					//Get previous frame numner in current chain
					 auto prevFrameInCurrentChain = extFrameNum - frameDiffsChains[chain];
					 //If it is not us, check if previus frame was not sent
					 if (prevFrameInCurrentChain!=currentFrameNumber && !forwardedFrames.Contains(prevFrameInCurrentChain))
						//Chain is broken, try next
						continue;
					
					//Got it
					currentChain = chain;
					currentDecodeTarget = decodeTarget;
					break;
				}
			}
		}
	}

	//If there is none available
	if (currentChain==NoChain || currentDecodeTarget==NoDecodeTarget)
	{
		//Request intra
		waitingForIntra = true;
		//Ignore packet
		return false;
	}
	
	//If frame is not decodable but we can't discard it
	if (!decodable && decodeTargetIndications[currentDecodeTarget]==DecodeTargetIndication::Discardable)
		//Ignore packet but do not discard packet
		return false;
	
	//RTP mark is set for the last frame layer of the selected spatial layer
	mark = packet->GetMark() || (dependencyDescriptor->endOfFrame && spatialLayerId==frameDependencyTemplate.spatialLayerId);
	
	//Not waiting for intra
	waitingForIntra = false;
	
	//If it is the last in current frame
	if (dependencyDescriptor->endOfFrame)
		//We only count full forwarded frames
		forwardedFrames.Add(currentFrameNumber);
	
	//UltraDebug("-DependencyDescriptorLayerSelector::Select() | Accepting packet [extSegNum:%u,mark:%d,sid:%d,tid:%d,current:S%dL%d]\n",packet->GetExtSeqNum(),mark,desc.spatialLayerId,desc.temporalLayerId,spatialLayerId,temporalLayerId);
	//Select
	return true;
	
}

 LayerInfo DependencyDescriptorLayerSelector::GetLayerIds(const RTPPacket::shared& packet)
{
	//Get dependency description
	auto& dependencyDescriptor = packet->GetDependencyDescriptor();
	auto& currentTemplateDependencyStructure = packet->GetTemplateDependencyStructure();
	
	//check 
	if (dependencyDescriptor 
		&& currentTemplateDependencyStructure
		&& currentTemplateDependencyStructure->ContainsFrameDependencyTemplate(dependencyDescriptor->frameDependencyTemplateId))
		//Get layer info from template
		return currentTemplateDependencyStructure->GetFrameDependencyTemplate(dependencyDescriptor->frameDependencyTemplateId);
	
	//Return empty layer info
	return LayerInfo();
}