/*
 * Copyright (c) 2008, AIST, the University of Tokyo and General Robotix Inc.
 * All rights reserved. This program is made available under the terms of the
 * Eclipse Public License v1.0 which accompanies this distribution, and is
 * available at http://www.eclipse.org/legal/epl-v10.html
 * Contributors:
 * National Institute of Advanced Industrial Science and Technology (AIST)
 * General Robotix Inc. 
 */
/**
   \file
   \author Shin'ichiro Nakaoka
*/

#include "Controller_impl.h"

#include <string>
#include <iostream>
#include <rtm/Manager.h>
#include <rtm/RTObject.h>
#include <rtm/NVUtil.h>

#include <hrpCorba/ORBwrap.h>

#include "VirtualRobotRTC.h"

using namespace std;
using namespace boost;

namespace {
    const bool CONTROLLER_BRIDGE_DEBUG = false;
}


Controller_impl::Controller_impl(RTC::Manager* rtcManager, BridgeConf* bridgeConf)
    :   rtcManager(rtcManager),
        bridgeConf(bridgeConf),
        modelName(""),
        bRestart(false)
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::Controller_impl" << endl;
    }
    VirtualRobotRTC::registerFactory(rtcManager, bridgeConf->getVirtualRobotRtcTypeName());

    RTC::RtcBase* rtc = rtcManager->createComponent(bridgeConf->getVirtualRobotRtcTypeName());
    virtualRobotRTC = dynamic_cast<VirtualRobotRTC*>(rtc);

}


Controller_impl::~Controller_impl()
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::~Controller_impl" << endl;
    }
    delete naming;

    virtualRobotRTC->isOwnedByController = false;
}


void Controller_impl::detectRtcs()
{
    RTC::Manager& rtcManager = RTC::Manager::instance();

    string nameServer = rtcManager.getConfig()["corba.nameservers"];
    cout << "setting naming" << endl;
    int comPos = nameServer.find(",");
    if (comPos < 0){
        comPos = nameServer.length();
    }
    nameServer = nameServer.substr(0, comPos);
    naming = new RTC::CorbaNaming(rtcManager.getORB(), nameServer.c_str());

    cout << "setup RT components" << endl;

    for(size_t i=0; i < bridgeConf->portConnections.size(); ++i){

        PortConnection& connection = bridgeConf->portConnections[i];

        RTC::RTObject_var rtcRef;
        string rtcName;

        if(connection.controllerInstanceName.empty()){
            if(!bridgeConf->moduleInfoList.empty()){
                RTC::RtcBase* rtcServant = bridgeConf->moduleInfoList.front().rtcServant;
                rtcName = "";                                                 
                rtcRef = rtcServant->getObjRef();
            }
        } else {
            rtcName = connection.controllerInstanceName;
        }
         
        RtcInfoMap::iterator it = rtcInfoMap.find(rtcName);
        if(it==rtcInfoMap.end()){
            if(rtcName!=""){
                string rtcNamingName = rtcName + ".rtc";
                CORBA::Object_var objRef = naming->resolve(rtcNamingName.c_str());
                if(CORBA::is_nil(objRef)){
                    cout << rtcName << " is not found." << endl;
                } else {
                    rtcRef = RTC::RTObject::_narrow(objRef);
                    if(CORBA::is_nil(rtcRef)){
                        cout << rtcName << " is not an RTC object." << endl;
                    }
                }
            }
            if(!CORBA::is_nil(rtcRef)){
                RtcInfoPtr rtcInfo = addRtcVectorWithConnection(rtcRef);
                rtcInfoMap.insert(make_pair(rtcName,rtcInfo));
            }
        }
    }

    RTC::RTCList_var rtcList = virtualRobotRTC->getConnectedRtcs();
    for(CORBA::ULong i=0; i < rtcList->length(); ++i){
        addRtcVectorWithConnection(rtcList[i]);
    }
}


void Controller_impl::makePortMap(RtcInfoPtr& rtcInfo)
{
	Port_Service_List_Var_Type ports = rtcInfo->rtcRef->get_ports();
    for(CORBA::ULong i=0; i < ports->length(); ++i){
        RTC::PortProfile_var profile = ports[i]->get_port_profile();
        rtcInfo->portMap[string(profile->name)] = ports[i];
    }
}

Controller_impl::RtcInfoPtr Controller_impl::addRtcVectorWithConnection(RTC::RTObject_var new_rtcRef)
{
    RtcInfoVector::iterator it = rtcInfoVector.begin();
    for( ; it != rtcInfoVector.end(); ++it){
        if((*it)->rtcRef->_is_equivalent(new_rtcRef))
            return *it;
    }

    RtcInfoPtr rtcInfo(new RtcInfo());
    rtcInfo->rtcRef = new_rtcRef;
    makePortMap(rtcInfo);
    rtcInfo->timeRate = 1.0;

    rtcInfoVector.push_back(rtcInfo);


#ifdef OPENRTM_VERSION_042
    RTC::ExecutionContextServiceList_var eclist = rtcInfo->rtcRef->get_execution_context_services();
    for(CORBA::ULong i=0; i < eclist->length(); ++i){
        if(!CORBA::is_nil(eclist[i])){
            rtcInfo->execContext = ExtTrigExecutionContextService_Type::_narrow(eclist[i]);
            if(!CORBA::is_nil(rtcInfo->execContext)){
                cout << "detected the ExtTrigExecutionContext" << endl;
            }
            break;
        }
    }
#else
    RTC::ExecutionContextList_var eclist = rtcInfo->rtcRef->get_owned_contexts();
    for(CORBA::ULong i=0; i < eclist->length(); ++i){
        if(!CORBA::is_nil(eclist[i])){
            rtcInfo->execContext = ExtTrigExecutionContextService_Type::_narrow(eclist[i]);
            if(!CORBA::is_nil(rtcInfo->execContext)){
                cout << "detected the ExtTrigExecutionContext" << endl;
            }
            break;
        }
    }
#endif
    return rtcInfo;
}

void Controller_impl::setupRtcConnections()
{
    for(size_t i=0; i < bridgeConf->portConnections.size(); ++i){

        const PortConnection& connection = bridgeConf->portConnections[i];

        string controllerInstanceName = connection.controllerInstanceName;
        if(controllerInstanceName.empty()){
            if(!bridgeConf->moduleInfoList.empty()){
                controllerInstanceName = "";
            }
        }

        cout << "connect " << virtualRobotRTC->getInstanceName() << ":" << connection.robotPortName;
        cout << " <--> " << controllerInstanceName << ":" << connection.controllerPortName;

        bool connected = false;

        RtcInfoMap::iterator p = rtcInfoMap.find(controllerInstanceName);
        if(p != rtcInfoMap.end()){
            RtcInfoPtr rtcInfo = p->second;

            PortMap::iterator q = rtcInfo->portMap.find(connection.controllerPortName);
            if(q == rtcInfo->portMap.end()){
                cout << "\n";
                cout << controllerInstanceName << " does not have a port ";
                cout << connection.controllerPortName << "\n";
            } else {
            	Port_Service_Ptr_Type controllerPortRef = q->second;

                PortHandlerPtr robotPortHandler = virtualRobotRTC->getPortHandler(connection.robotPortName);
                if(!robotPortHandler){
                    cout << "\n";
                    cout << "The robot does not have a port named " << connection.robotPortName << "\n";
                } else {
                	Port_Service_Ptr_Type robotPortRef = robotPortHandler->portRef;

                    if(!CORBA::is_nil(robotPortRef)){
                        if(dynamic_pointer_cast<OutPortHandler>(robotPortHandler)){
                            connected = connectPorts(robotPortRef, controllerPortRef);
                        } else {
                            connected = connectPorts(controllerPortRef, robotPortRef);
                        }
                    }
                }
            }
        }

        if(connected){
            cout << " ...ok" << endl;
        } else {
            cout << "Connection failed." << endl;
        }
    }
}


bool Controller_impl::connectPorts(Port_Service_Ptr_Type outPort, Port_Service_Ptr_Type inPort)
{
    // connect ports
    RTC::ConnectorProfile cprof;
    cprof.connector_id = "";
    cprof.name = CORBA::string_dup("connector0");
    cprof.ports.length(2);
    cprof.ports[0] = Port_Service_Type::_duplicate(inPort);
    cprof.ports[1] = Port_Service_Type::_duplicate(outPort);

    CORBA_SeqUtil::push_back(cprof.properties,
		       NVUtil::newNV("dataport.dataflow_type",
				     "Push"));
#ifdef OPENRTM_VERSION_042
    CORBA_SeqUtil::push_back(cprof.properties,
		       NVUtil::newNV("dataport.interface_type",
				     "CORBA_Any"));
    CORBA_SeqUtil::push_back(cprof.properties,
		       NVUtil::newNV("dataport.subscription_type",
				     "Flush"));
#else
    CORBA_SeqUtil::push_back(cprof.properties,
		       NVUtil::newNV("dataport.interface_type",
				     "corba_cdr"));
    CORBA_SeqUtil::push_back(cprof.properties,
		       NVUtil::newNV("dataport.subscription_type",
				     "flush"));
#endif
    RTC::ReturnCode_t result = inPort->connect(cprof);

    return (result == RTC::RTC_OK);
}


void Controller_impl::setDynamicsSimulator(DynamicsSimulator_ptr dynamicsSimulator)
{
    this->dynamicsSimulator = DynamicsSimulator::_duplicate(dynamicsSimulator);
}


void Controller_impl::setViewSimulator(ViewSimulator_ptr viewSimulator)
{
    this->viewSimulator = ViewSimulator::_duplicate(viewSimulator);
}


void Controller_impl::start()
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::onStart" << endl;
    }

    controlTime = 0.0;
    try{
        if( bRestart ){
            restart();
        } else {
            if(!CORBA::is_nil(viewSimulator)) {
                viewSimulator->getCameraSequenceOf(modelName.c_str(), cameras);
            }else{
                cameras = new CameraSequence(0);
            }
            activeComponents();
        }
    } catch(CORBA_SystemException& ex){
        cerr << ex._rep_id() << endl;
        cerr << "exception in Controller_impl::start" << endl;
    } catch(...){
        cerr << "unknown exception in Controller_impl::start()" <<  endl;
    }
}


SensorState& Controller_impl::getCurrentSensorState()
{
    if(!sensorStateUpdated){
        dynamicsSimulator->getCharacterSensorState(modelName.c_str(), sensorState);
        sensorStateUpdated = true;
    }

    return sensorState;
}


DblSequence* Controller_impl::getLinkDataFromSimulator
(const std::string& linkName, DynamicsSimulator::LinkDataType linkDataType)
{
    DblSequence_var data;
    dynamicsSimulator->getCharacterLinkData(modelName.c_str(), linkName.c_str(), linkDataType, data.out());
    return data._retn();
}


DblSequence* Controller_impl::getSensorDataFromSimulator(const std::string& sensorName)
{
    DblSequence_var data;
    dynamicsSimulator->getCharacterSensorValues(modelName.c_str(), sensorName.c_str(), data.out());
    return data._retn();
}


ImageData* Controller_impl::getCameraImageFromSimulator(int cameraId)
{
    if(cameras->length()!=0){
        ImageData_var imageData = cameras[cameraId]->getImageData();
        return imageData._retn();
    }else{
        ImageData* imageData = new ImageData;
        imageData->floatData.length(0);
        imageData->longData.length(0);
        imageData->octetData.length(0);
        return imageData;
    }
}


void Controller_impl::input()
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::onInput" << endl;
    }

    sensorStateUpdated = false;

    virtualRobotRTC->inputDataFromSimulator(this);
}


DblSequence& Controller_impl::getJointDataSeqRef(DynamicsSimulator::LinkDataType linkDataType)
{
    return outputJointValueSeqInfos[linkDataType].values;
}


void Controller_impl::flushJointDataSeqToSimulator(DynamicsSimulator::LinkDataType linkDataType)
{
    JointValueSeqInfoMap::iterator p = outputJointValueSeqInfos.find(linkDataType);
    if(p != outputJointValueSeqInfos.end()){
        JointValueSeqInfo& info = p->second;
        if(!info.flushed){
            dynamicsSimulator->setCharacterAllLinkData(modelName.c_str(), linkDataType, info.values);
            info.flushed = true;
        }
    }
}

void Controller_impl::flushLinkDataToSimulator(const std::string& linkName,
					       DynamicsSimulator::LinkDataType linkDataType,
					       const DblSequence& linkData)
{
    dynamicsSimulator->setCharacterLinkData(modelName.c_str(), linkName.c_str(),
					linkDataType, linkData);
}

void Controller_impl::output()
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::output" << endl;
    }

    for(JointValueSeqInfoMap::iterator p = outputJointValueSeqInfos.begin();
      p != outputJointValueSeqInfos.end(); ++p){
        JointValueSeqInfo& info = p->second;
        info.flushed = false;
    }

    virtualRobotRTC->outputDataToSimulator(this);
}


void Controller_impl::control()
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::control" << endl;
    }

  virtualRobotRTC->writeDataToOutPorts(this);

    for(RtcInfoVector::iterator p = rtcInfoVector.begin(); p != rtcInfoVector.end(); ++p){
        RtcInfoPtr& rtcInfo = *p;
        if(!CORBA::is_nil(rtcInfo->execContext)){
            rtcInfo->timeRateCounter += rtcInfo->timeRate;
            if(rtcInfo->timeRateCounter >= 1.0){
                rtcInfo->execContext->tick();
                rtcInfo->timeRateCounter -= 1.0;
            }
        }
    }

    virtualRobotRTC->readDataFromInPorts(this);

  controlTime += timeStep;
}


void Controller_impl::stop()
{
    deactiveComponents();
}


void Controller_impl::destroy()
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::destroy()" << endl;
    }
    PortableServer::POA_var poa = _default_POA();
    PortableServer::ObjectId_var id = poa->servant_to_id(this);
    poa->deactivate_object(id);
}

// TAWARA INSERT CODE 2009/01/14 START

void Controller_impl::initilize()
{
    if( virtualRobotRTC)
    {
        if( virtualRobotRTC->isOwnedByController ){
            bRestart = false;
        } else {
            virtualRobotRTC->isOwnedByController = true;
            try{
                detectRtcs();
                setupRtcConnections();
            } catch(CORBA_SystemException& ex){
                cerr << ex._rep_id() << endl;
                cerr << "exception in initilizeController" << endl;
            } catch(std::invalid_argument& ex){
                cerr << "invalid argument : " << ex.what() << endl;
            } catch(...){
                cerr << "unknown exception in Controller_impl::initilize()" <<  endl;
            }
        }
    }
}
void Controller_impl::shutdown()
{
    if(CONTROLLER_BRIDGE_DEBUG){
        cout << "Controller_impl::shutdown()" << endl;
    }
    destroy();
    rtcManager->terminate();
}

void Controller_impl::restart()
{
    activeComponents();
}
void Controller_impl::activeComponents()
{
    for(RtcInfoVector::iterator p = rtcInfoVector.begin(); p != rtcInfoVector.end(); ++p){
        RtcInfoPtr& rtcInfo = *p;
        rtcInfo->timeRateCounter = 1.0;
        if(!CORBA::is_nil(rtcInfo->execContext)){
            if( RTC::PRECONDITION_NOT_MET == rtcInfo->execContext->activate_component(rtcInfo->rtcRef) )
            {
                rtcInfo->execContext->reset_component(rtcInfo->rtcRef);
                rtcInfo->execContext->activate_component(rtcInfo->rtcRef);
            }
        }
    }
}

void Controller_impl::deactiveComponents()
{
    std::vector<ExtTrigExecutionContextService_Var_Type> vecExecContext;
    for(RtcInfoVector::iterator p = rtcInfoVector.begin(); p != rtcInfoVector.end(); ++p){
        RtcInfoPtr& rtcInfo = *p;
        rtcInfo->timeRateCounter = 1.0;
        if(!CORBA::is_nil(rtcInfo->execContext)){
            if ( RTC::RTC_OK == rtcInfo->execContext->deactivate_component(rtcInfo->rtcRef) ){
                vecExecContext.push_back(rtcInfo->execContext);
            }
        }
    }
    for( std::vector<ExtTrigExecutionContextService_Var_Type>::iterator ite = vecExecContext.begin();
         ite != vecExecContext.end();   ++ite   ){
        if(!CORBA::is_nil( *ite )){
            try{
                // Trigger onDeactivated in component.
                (*ite)->tick();
            } catch ( CORBA_SystemException& ex){
                cerr << ex._rep_id() << endl;
                cerr << "exception in Controller_impl::deactiveComponents" << endl;
            } catch (...) {
                cerr << "unknown exception in Controller_impl::initilize()" <<  endl;
            }
        }
    }
}
