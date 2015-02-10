#ifndef MAMMUT_FASTFLOW_TPP_
#define MAMMUT_FASTFLOW_TPP_

#include <limits>

namespace mammut{
namespace fastflow{

template <typename lb_t, typename gt_t>
std::vector<AdaptiveNode*> AdaptiveFarm<lb_t, gt_t>::getAdaptiveWorkers() const{
    return _adaptiveWorkers;
}

template <typename lb_t, typename gt_t>
AdaptiveNode* AdaptiveFarm<lb_t, gt_t>::getAdaptiveEmitter() const{
    return _adaptiveEmitter;
}

template <typename lb_t, typename gt_t>
AdaptiveNode* AdaptiveFarm<lb_t, gt_t>::getAdaptiveCollector() const{
    return _adaptiveCollector;
}

template <typename lb_t, typename gt_t>
void AdaptiveFarm<lb_t, gt_t>::construct(AdaptivityParameters* adaptivityParameters){
    _adaptiveEmitter = NULL;
    _adaptiveCollector = NULL;
    _firstRun = true;
    _adaptivityParameters = adaptivityParameters;
    _adaptivityManager = NULL;
    uint validationRes = _adaptivityParameters->validate();
    if(validationRes != VALIDATION_OK){
        throw std::runtime_error("AdaptiveFarm: invalid AdaptivityParameters: " + utils::intToString(validationRes));
    }
}

template <typename lb_t, typename gt_t>
AdaptiveFarm<lb_t, gt_t>::AdaptiveFarm(AdaptivityParameters* adaptivityParameters, std::vector<ff_node*>& w,
                                       ff_node* const emitter, ff_node* const collector, bool inputCh):
    ff_farm<lb_t, gt_t>::ff_farm(w, emitter, collector, inputCh){
    construct(adaptivityParameters);
}

template <typename lb_t, typename gt_t>
AdaptiveFarm<lb_t, gt_t>::AdaptiveFarm(AdaptivityParameters* adaptivityParameters, bool inputCh,
                                       int inBufferEntries,
                                       int outBufferEntries,
                                       bool workerCleanup,
                                       int maxNumWorkers,
                                       bool fixedSize):
    ff_farm<lb_t, gt_t>::ff_farm(inputCh, inBufferEntries, outBufferEntries, workerCleanup, maxNumWorkers, fixedSize){
    construct(adaptivityParameters);
}

template <typename lb_t, typename gt_t>
AdaptiveFarm<lb_t, gt_t>::~AdaptiveFarm(){
    ;
}

template <typename lb_t, typename gt_t>
int AdaptiveFarm<lb_t, gt_t>::run(bool skip_init){
    if(_firstRun){
        svector<ff_node*> workers = ff_farm<lb_t, gt_t>::getWorkers();
        for(size_t i = 0; i < workers.size(); i++){
            _adaptiveWorkers.push_back(static_cast<AdaptiveNode*>(workers[i]));
            _adaptiveWorkers.at(i)->initMammutModules(_adaptivityParameters->communicator);
        }

        _adaptiveEmitter = static_cast<AdaptiveNode*>(ff_farm<lb_t, gt_t>::getEmitter());
        if(_adaptiveEmitter){
            _adaptiveEmitter->initMammutModules(_adaptivityParameters->communicator);
        }

        _adaptiveCollector = static_cast<AdaptiveNode*>(ff_farm<lb_t, gt_t>::getCollector());
        if(_adaptiveCollector){
            _adaptiveCollector->initMammutModules(_adaptivityParameters->communicator);
        }
    }

    int r = ff_farm<lb_t, gt_t>::run(skip_init);
    if(r){
        return r;
    }

    if(_firstRun){
        _firstRun = false;
        _adaptivityManager = new AdaptivityManagerFarm<lb_t, gt_t>(this, _adaptivityParameters);
        _adaptivityManager->start();
    }
    std::cout << "Run adaptive farm." << std::endl;
    return r;
}

template <typename lb_t, typename gt_t>
int AdaptiveFarm<lb_t, gt_t>::wait(){
    if(_adaptivityManager){
        _adaptivityManager->stop();
        _adaptivityManager->join();
        delete _adaptivityManager;
    }
    return ff_farm<lb_t, gt_t>::wait();
}

template<typename lb_t, typename gt_t>
AdaptivityManagerFarm<lb_t, gt_t>::AdaptivityManagerFarm(AdaptiveFarm<lb_t, gt_t>* farm, AdaptivityParameters* adaptivityParameters):
    _stop(false),
    _farm(farm),
    _p(adaptivityParameters),
    _workers(_farm->getAdaptiveWorkers()),
    _maxNumWorkers(_workers.size()),
    _currentNumWorkers(_workers.size()),
    _currentFrequency(0),
    _emitterVirtualCore(NULL),
    _collectorVirtualCore(NULL){
    ;
}

template<typename lb_t, typename gt_t>
AdaptivityManagerFarm<lb_t, gt_t>::~AdaptivityManagerFarm(){
    ;
}

template<typename lb_t, typename gt_t>
std::vector<topology::PhysicalCore*> AdaptivityManagerFarm<lb_t, gt_t>::getSeparatedDomainPhysicalCores(const std::vector<topology::VirtualCore*>& virtualCores) const{
    std::vector<cpufreq::Domain*> allDomains = _p->cpufreq->getDomains();
    std::vector<cpufreq::Domain*> hypotheticWorkersDomains = _p->cpufreq->getDomains(virtualCores);
    std::vector<topology::PhysicalCore*> physicalCoresInUnusedDomains;
    if(allDomains.size() > hypotheticWorkersDomains.size()){
       for(size_t i = 0; i < allDomains.size(); i++){
           cpufreq::Domain* currentDomain = allDomains.at(i);
           if(!utils::contains(hypotheticWorkersDomains, currentDomain)){
               utils::append(physicalCoresInUnusedDomains,
                             _p->topology->virtualToPhysical(currentDomain->getVirtualCores()));
           }
       }
    }
    return physicalCoresInUnusedDomains;
}

template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::setVirtualCoreToHighestFrequency(topology::VirtualCore* virtualCore){
    cpufreq::Domain* performanceDomain = _p->cpufreq->getDomain(virtualCore);
    bool frequencySet = performanceDomain->setGovernor(mammut::cpufreq::GOVERNOR_PERFORMANCE);
    if(!frequencySet){
        if(!performanceDomain->setGovernor(mammut::cpufreq::GOVERNOR_USERSPACE) ||
           !performanceDomain->setHighestFrequencyUserspace()){
            throw std::runtime_error("AdaptivityManagerFarm: Fatal error while setting highest frequency for "
                                     "sensitive emitter/collector. Try to run it without sensitivity parameters.");
        }
    }
}

template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::setUnusedVirtualCores(){
    switch(_p->strategyMapping){
        case STRATEGY_MAPPING_AUTO: //TODO: Auto should choose between all the others
        case STRATEGY_MAPPING_LINEAR:{
           /*
            * Generates a vector of virtual cores to be used for linear mapping.node
            * It contains first one virtual core per physical core (virtual cores
            * on the same CPU are consecutive).
            * Then, the other groups of virtual cores follow.
            */
            std::vector<topology::Cpu*> cpus = _p->topology->getCpus();

            size_t virtualUsed = 0;
            size_t virtualPerPhysical = _p->topology->getVirtualCores().size() /
                                        _p->topology->getPhysicalCores().size();
            while(virtualUsed < virtualPerPhysical){
                for(size_t i = 0; i < cpus.size(); i++){
                    std::vector<topology::PhysicalCore*> physicalCores = cpus.at(i)->getPhysicalCores();
                    for(size_t j = 0; j < physicalCores.size(); j++){
                        _unusedVirtualCores.push_back(physicalCores.at(j)->getVirtualCores().at(virtualUsed));
                    }
                }
                ++virtualUsed;
            }
        }break;
        case STRATEGY_MAPPING_CACHE_EFFICIENT:{
            throw std::runtime_error("Not yet supported.");
        }
        default:
            break;
    }
}

template<typename lb_t, typename gt_t>
StrategyUnusedVirtualCores AdaptivityManagerFarm<lb_t, gt_t>::
                           computeAutoUnusedVCStrategy(const std::vector<topology::VirtualCore*>& virtualCores){
    for(size_t i = 0; i < virtualCores.size(); i++){
        /** If at least one is hotpluggable we apply the VC_OFF strategy. **/
        if(virtualCores.at(i)->isHotPluggable()){
            return STRATEGY_UNUSED_VC_OFF;
        }
    }

    if((_p->cpufreq->isGovernorAvailable(cpufreq::GOVERNOR_POWERSAVE) ||
        _p->cpufreq->isGovernorAvailable(cpufreq::GOVERNOR_USERSPACE))){
        return STRATEGY_UNUSED_VC_LOWEST_FREQUENCY;
    }
    return STRATEGY_UNUSED_VC_NONE;
}

template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::applyUnusedVirtualCoresStrategy(StrategyUnusedVirtualCores strategyUnusedVirtualCores,
                                                                        const std::vector<topology::VirtualCore*>& virtualCores){
    switch(_p->strategyNeverUsedVirtualCores){
        case STRATEGY_UNUSED_VC_OFF:{
            for(size_t i = 0; i < virtualCores.size(); i++){
                topology::VirtualCore* vc = virtualCores.at(i);
                if(vc->isHotPluggable()){
                    vc->hotUnplug();
                }
            }
        }break;
        case STRATEGY_UNUSED_VC_LOWEST_FREQUENCY:{
            std::vector<cpufreq::Domain*> unusedDomains = _p->cpufreq->getDomainsComplete(virtualCores);
            for(size_t i = 0; i < unusedDomains.size(); i++){
                cpufreq::Domain* domain = unusedDomains.at(i);
                if(!domain->setGovernor(cpufreq::GOVERNOR_POWERSAVE)){
                    if(!domain->setGovernor(cpufreq::GOVERNOR_USERSPACE) ||
                       !domain->setLowestFrequencyUserspace()){
                        throw std::runtime_error("AdaptivityManagerFarm: Impossible to set lowest frequency "
                                                 "for unused virtual cores.");
                    }
                }
            }
        }break;
        default:{
            return;
        }
    }
}

template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::updatePstate(const std::vector<topology::VirtualCore*>& virtualCores,
                                                     cpufreq::Frequency frequency){
    /**
     *  We only change and set frequency to domains that contain at
     *  least one used virtual core.
     **/
    std::vector<cpufreq::Domain*> usedDomains = _p->cpufreq->getDomains(virtualCores);

    for(size_t i = 0; i < usedDomains.size(); i++){
        if(!usedDomains.at(i)->setGovernor(_p->frequencyGovernor)){
            throw std::runtime_error("AdaptivityManagerFarm: Impossible to set the specified governor.");
        }
        if(_p->frequencyGovernor != cpufreq::GOVERNOR_USERSPACE){
            if(!usedDomains.at(i)->setGovernorBounds(_p->frequencyLowerBound,
                                                 _p->frequencyUpperBound)){
                throw std::runtime_error("AdaptivityManagerFarm: Impossible to set the specified governor's bounds.");;
            }
        }else if(_p->strategyFrequencies != STRATEGY_FREQUENCY_OS){
            if(!usedDomains.at(i)->setFrequencyUserspace(frequency)){
                throw std::runtime_error("AdaptivityManagerFarm: Impossible to set the specified frequency.");;
            }
        }
    }
}

template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::mapAndSetFrequencies(){
    if(_p->strategyMapping == STRATEGY_MAPPING_NO){
        return;
    }
    /** Computes map. **/
    setUnusedVirtualCores();

    bool emitterMappingRequired = (_farm->getEmitter()!=NULL);
    bool collectorMappingRequired = (_farm->getCollector()!=NULL);
    std::vector<topology::VirtualCore*> frequencyScalableVirtualCores;

    /**
     * If requested, and if there are available domains, run emitter or collector (or both) at
     * the highest frequency.
     */
    if(_p->strategyFrequencies != STRATEGY_FREQUENCY_NO &&
      (_p->sensitiveEmitter || _p->sensitiveCollector)){
        size_t scalableVirtualCoresNum = _workers.size() +
                                      (emitterMappingRequired && !_p->sensitiveEmitter)?1:0 +
                                      (collectorMappingRequired && !_p->sensitiveCollector)?1:0;
        /** When sensitive is specified, we always choose the WEC mapping. **/
        std::vector<topology::VirtualCore*> scalableVirtualCores(_unusedVirtualCores.begin(), (scalableVirtualCoresNum < _unusedVirtualCores.size())?
                                                                                                   _unusedVirtualCores.begin() + scalableVirtualCoresNum:
                                                                                                   _unusedVirtualCores.end());
        std::vector<topology::PhysicalCore*> performancePhysicalCores = getSeparatedDomainPhysicalCores(scalableVirtualCores);
        if(performancePhysicalCores.size()){
            size_t index = 0;

            if(_p->sensitiveEmitter && emitterMappingRequired){
                topology::VirtualCore* vc = performancePhysicalCores.at(index)->getVirtualCore();
                setVirtualCoreToHighestFrequency(vc);
                _emitterVirtualCore = vc;
                emitterMappingRequired = false;
                index = (index + 1) % performancePhysicalCores.size();
            }

            if(_p->sensitiveCollector && collectorMappingRequired){
                topology::VirtualCore* vc = performancePhysicalCores.at(index)->getVirtualCore();
                setVirtualCoreToHighestFrequency(vc);
                _collectorVirtualCore = vc;
                collectorMappingRequired = false;
                index = (index + 1) % performancePhysicalCores.size();
            }
        }
    }

    //TODO: Better to map [w-w-w-w-w......-w-w-E-C], [E-w-w-w-....w-w-w-C] or
    //                    [E-C-w-w-w-.......-w-w]? (first and third are the same only if we have fully used CPUs)
    // Now EWC is always applied
    if(emitterMappingRequired){
        _emitterVirtualCore = _unusedVirtualCores.front();
        _unusedVirtualCores.erase(_unusedVirtualCores.begin());
        frequencyScalableVirtualCores.push_back(_emitterVirtualCore);
    }

    for(size_t i = 0; i < _workers.size(); i++){
        topology::VirtualCore* vc = _unusedVirtualCores.at(i);
        _workersVirtualCores.push_back(vc);
        frequencyScalableVirtualCores.push_back(vc);
    }
    _unusedVirtualCores.erase(_unusedVirtualCores.begin(), _unusedVirtualCores.begin() + _workers.size());

    if(collectorMappingRequired){
        _collectorVirtualCore = _unusedVirtualCores.front();
        _unusedVirtualCores.erase(_unusedVirtualCores.begin());
        frequencyScalableVirtualCores.push_back(_collectorVirtualCore);
    }

    /** Perform mapping. **/
    AdaptiveNode* node = NULL;
    //TODO: Che succede se la farm ha l'emitter di default? :(
    if((node = _farm->getAdaptiveEmitter())){
        node->getThreadHandler()->move(_emitterVirtualCore);
    }

    if((node = _farm->getAdaptiveCollector())){
        node->getThreadHandler()->move(_collectorVirtualCore);
    }

    for(size_t i = 0; i < _workers.size(); i++){
        _workers.at(i)->getThreadHandler()->move(_workersVirtualCores.at(i));
    }

    if(_p->strategyNeverUsedVirtualCores == STRATEGY_UNUSED_VC_AUTO){
        _p->strategyNeverUsedVirtualCores = computeAutoUnusedVCStrategy(_unusedVirtualCores);
    }
    applyUnusedVirtualCoresStrategy(_p->strategyNeverUsedVirtualCores,
                                    _unusedVirtualCores);

    if(_p->strategyFrequencies != STRATEGY_FREQUENCY_NO && _p->strategyFrequencies != STRATEGY_FREQUENCY_OS){
        /** We suppose that all the domains have the same available frequencies. **/
        _availableFrequencies = _p->cpufreq->getDomains().at(0)->getAvailableFrequencies();
        _currentFrequency = _availableFrequencies.back(); // Sets the current frequency to the highest possible.
        updatePstate(frequencyScalableVirtualCores, _currentFrequency);
    }
}

template<typename lb_t, typename gt_t>
double AdaptivityManagerFarm<lb_t, gt_t>::getWorkerAverageLoad(size_t workerId){
    double r = 0;
    for(size_t i = 0; i < _p->numSamples; i++){
        r += _nodeSamples.at(workerId).at(i).loadPercentage;
    }
    return r / ((double) _p->numSamples);
}

template<typename lb_t, typename gt_t>
double AdaptivityManagerFarm<lb_t, gt_t>::getFarmAverageLoad(){
    double r = 0;
    for(size_t i = 0; i < _currentNumWorkers; i++){
        r += getWorkerAverageLoad(i);
    }
    return r / _currentNumWorkers;
}

template<typename lb_t, typename gt_t>
double AdaptivityManagerFarm<lb_t, gt_t>::getWorkerAverageBandwidth(size_t workerId){
    double r = 0;
    for(size_t i = 0; i < _p->numSamples; i++){
        r += _nodeSamples.at(workerId).at(i).tasksCount;
    }
    return r / ((double) _p->numSamples * (double) _p->samplingInterval);
}

template<typename lb_t, typename gt_t>
double AdaptivityManagerFarm<lb_t, gt_t>::getFarmAverageBandwidth(){
    double r = 0;
    for(size_t i = 0; i < _currentNumWorkers; i++){
        r += getWorkerAverageBandwidth(i);
    }
    return r;
}

template<typename lb_t, typename gt_t>
double AdaptivityManagerFarm<lb_t, gt_t>::getMonitoredValue(){
    if(_p->requiredBandwidth){
        return getFarmAverageBandwidth();
    }else{
        return getFarmAverageLoad();
    }
}

template<typename lb_t, typename gt_t>
bool AdaptivityManagerFarm<lb_t, gt_t>::isContractViolated(double monitoredValue){
    if(_p->requiredBandwidth){
        double offset = (_p->requiredBandwidth * _p->maxBandwidthVariation) / 100.0;
        return monitoredValue < _p->requiredBandwidth - offset ||
               monitoredValue > _p->requiredBandwidth + offset;
    }else{
        return monitoredValue < _p->underloadThresholdFarm ||
               monitoredValue > _p->overloadThresholdFarm;
    }
}

template<typename lb_t, typename gt_t>
double AdaptivityManagerFarm<lb_t, gt_t>::getEstimatedMonitoredValue(double monitoredValue, cpufreq::Frequency frequency, uint numWorkers){
    if(_p->requiredBandwidth){
        return monitoredValue * ((frequency * numWorkers) / _currentFrequency * _currentNumWorkers);
    }else{
        return monitoredValue * ((_currentFrequency * _currentNumWorkers) / (frequency * numWorkers));
    }
}

template<typename lb_t, typename gt_t>
double AdaptivityManagerFarm<lb_t, gt_t>::getEstimatedPower(cpufreq::Frequency frequency, uint numWorkers){
    throw std::runtime_error("Notimplemented.");
}


template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::getNewConfiguration(double monitoredValue, cpufreq::Frequency& frequency, uint& numWorkers){
    double minEstimatedPower = std::numeric_limits<double>::max();
    double estimatedPower = 0;
    double estimatedMonitoredValue = 0;
    cpufreq::Frequency examinedFrequency;
    for(size_t i = 0; i < _maxNumWorkers; i++){
        for(size_t j = 0; j < _availableFrequencies.size(); j++){
            examinedFrequency = _availableFrequencies.at(j);
            estimatedMonitoredValue = getEstimatedMonitoredValue(monitoredValue, examinedFrequency, i);
            if(!isContractViolated(estimatedMonitoredValue)){
                estimatedPower = getEstimatedPower(examinedFrequency, i);
                if(estimatedPower < minEstimatedPower){
                    minEstimatedPower = estimatedPower;
                    frequency = examinedFrequency;
                    numWorkers = i;
                }
            }
        }
    }
}


template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::run(){
    /**
     * Wait for all the nodes to be running.
     */
    std::vector<AdaptiveNode*> adaptiveWorkers = _farm->getAdaptiveWorkers();
    AdaptiveNode* adaptiveEmitter = _farm->getAdaptiveEmitter();
    AdaptiveNode* adaptiveCollector = _farm->getAdaptiveCollector();
    for(size_t i = 0; i < adaptiveWorkers.size(); i++){
        adaptiveWorkers.at(i)->waitThreadCreation();
    }
    if(adaptiveEmitter){
        adaptiveEmitter->waitThreadCreation();
    }
    if(adaptiveCollector){
        adaptiveCollector->waitThreadCreation();
    }

    double x = utils::getMillisecondsTime();
    mapAndSetFrequencies();
    std::cout << "Milliseconds elapsed: " << utils::getMillisecondsTime() - x << std::endl;

    std::cout << "Emitter node: " << _emitterVirtualCore->getVirtualCoreId() << std::endl;
    std::cout << "Collector node: " << _collectorVirtualCore->getVirtualCoreId() << std::endl;
    std::cout << "Worker nodes: ";
    for(size_t i = 0; i < adaptiveWorkers.size(); i++){
        std::cout <<  _workersVirtualCores.at(i)->getVirtualCoreId() << ", ";
    }
    std::cout << std::endl;

    _nodeSamples.resize(_workers.size());
    for(size_t i = 0; i < _workers.size(); i++){
        _nodeSamples.at(i).resize(_p->numSamples);
    }

    _lock.lock();
    size_t nextSampleIndex = 0;
    while(!_stop){
        _lock.unlock();
        std::cout << "Manager running" << std::endl;
        sleep(_p->samplingInterval);
        for(size_t i = 0; i < _currentNumWorkers; i++){
            _nodeSamples.at(i).at(nextSampleIndex) = _workers.at(i)->getAndResetSample();
        }
        nextSampleIndex = (nextSampleIndex + 1) % _p->numSamples;

        double monitoredValue = getMonitoredValue();
        if(isContractViolated(monitoredValue)){
            cpufreq::Frequency newFrequency;
            uint newWorkersNumber;
        }

        _lock.lock();
    }
    _lock.unlock();
}

template<typename lb_t, typename gt_t>
void AdaptivityManagerFarm<lb_t, gt_t>::stop(){
    _lock.lock();
    _stop = true;
    _lock.unlock();
}

}
}

#endif
