package k8sclient

import (
	"context"
	"fmt"
	"sync"
	"time"

	meta "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/klog/v2"

	"k8s.innull.com/trafficlimitd/pkg/types"
)

const defaultCacheExpiration = 10 * time.Minute

type K8sTrafficLimitInfoFetcher struct {
	clientset    *kubernetes.Clientset
	podInfoCache sync.Map
	closeChan    chan struct{}
}

type podCacheItem struct {
	podName, podNamespace string
	trafficLimitInfos     map[string]types.TrafficLimitInfo
	valid                 bool
	expireTime            time.Time
	cacheLockChan         chan struct{}
}

func NewK8sTrafficLimitInfoFetcher(clientset *kubernetes.Clientset) *K8sTrafficLimitInfoFetcher {
	result := &K8sTrafficLimitInfoFetcher{
		clientset: clientset,
		closeChan: make(chan struct{}),
	}
	result.startCleanTimer()
	return result
}

func (f *K8sTrafficLimitInfoFetcher) Close() {
	close(f.closeChan)
}

func (f *K8sTrafficLimitInfoFetcher) startCleanTimer() {
	go func() {
		ticker := time.NewTicker(2 * defaultCacheExpiration)
		for {
			select {
			case <-f.closeChan:
				ticker.Stop()
				return
			case <-ticker.C:
				f.deleteExpiredCache()
			}
		}
	}()
}

func (f *K8sTrafficLimitInfoFetcher) deleteExpiredCache() {
	now := time.Now()
	f.podInfoCache.Range(func(key, value interface{}) bool {
		podCacheItem := value.(*podCacheItem)
		if podCacheItem.expireTime.Before(now) {
			f.podInfoCache.Delete(key)
		}
		return true
	})
}

func (f *K8sTrafficLimitInfoFetcher) GetTrafficLimitInfo(ctx context.Context, containerHandle types.ContainerHandle) (types.TrafficLimitInfo, error) {
	item, ok := f.podInfoCache.LoadOrStore(containerHandle.PodUUID, &podCacheItem{
		valid:         false,
		cacheLockChan: make(chan struct{}),
		expireTime:    time.Now().Add(defaultCacheExpiration),
	})
	podCacheItem := item.(*podCacheItem)
	if !ok {
		defer close(podCacheItem.cacheLockChan)
		pod, err := f.clientset.CoreV1().Pods(containerHandle.PodNamespace).Get(ctx, containerHandle.PodName, meta.GetOptions{})
		if err != nil {
			f.podInfoCache.Delete(containerHandle.PodUUID)
			return types.TrafficLimitInfo{}, fmt.Errorf("failed to get pod %s(%s/%s): %v", containerHandle.PodUUID, containerHandle.PodNamespace, containerHandle.PodName, err)
		}
		klog.V(2).Infof("Get pod %s(%s/%s)", pod.UID, pod.Namespace, pod.Name)
		if containerHandle.PodNamespace != pod.Namespace || containerHandle.PodName != pod.Name || containerHandle.PodUUID != string(pod.UID) {
			f.podInfoCache.Delete(containerHandle.PodUUID)
			return types.TrafficLimitInfo{}, fmt.Errorf("pod %s(%s/%s) not found", containerHandle.PodUUID, containerHandle.PodNamespace, containerHandle.PodName)
		}
		podCacheItem.podNamespace = pod.Namespace
		podCacheItem.podName = pod.Name
		podCacheItem.valid = true
		podCacheItem.trafficLimitInfos = make(map[string]types.TrafficLimitInfo)
		for _, container := range pod.Spec.Containers {
			trafficLimitInfo := types.TrafficLimitInfo{}
			if container.Resources.Limits != nil {
				if bytesPerSecond, ok := container.Resources.Limits[types.BytesPerSecondResourceName]; ok {
					trafficLimitInfo.BytesPerSecond = bytesPerSecond.Value()
				}
				if packetsPerSecond, ok := container.Resources.Limits[types.PacketsPerSecondResourceName]; ok {
					trafficLimitInfo.PacketsPerSecond = packetsPerSecond.Value()
				}
			}
			podCacheItem.trafficLimitInfos[container.Name] = trafficLimitInfo
		}
		trafficLimitInfo, ok := podCacheItem.trafficLimitInfos[containerHandle.ContainerName]
		if !ok {
			return types.TrafficLimitInfo{}, nil
		} else {
			return trafficLimitInfo, nil
		}
	} else {
		<-podCacheItem.cacheLockChan
		if !podCacheItem.valid {
			return f.GetTrafficLimitInfo(ctx, containerHandle)
		}
		if podCacheItem.podNamespace != containerHandle.PodNamespace || podCacheItem.podName != containerHandle.PodName {
			return types.TrafficLimitInfo{}, fmt.Errorf("pod %s(%s/%s) not found", containerHandle.PodUUID, containerHandle.PodNamespace, containerHandle.PodName)
		}
		trafficLimitInfo, ok := podCacheItem.trafficLimitInfos[containerHandle.ContainerName]
		if !ok {
			return types.TrafficLimitInfo{}, nil
		}
		return trafficLimitInfo, nil
	}
}
