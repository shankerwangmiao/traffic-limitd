package types

import (
	"context"
	"fmt"
)

const BytesPerSecondResourceName = "trafficlimitd.innull.com/bytes-per-second"
const PacketsPerSecondResourceName = "trafficlimitd.innull.com/packets-per-second"

type ContainerHandle struct {
	PodUUID, PodNamespace, PodName, ContainerName string
}

type TrafficLimitInfo struct {
	BytesPerSecond   int64
	PacketsPerSecond int64
}

func (info *TrafficLimitInfo) String() string {
	return fmt.Sprint("(Bps:", info.BytesPerSecond, "pps:", info.PacketsPerSecond, ")")
}

type TrafficLimitInfoFetcher interface {
	GetTrafficLimitInfo(ctx context.Context, containerHandle ContainerHandle) (TrafficLimitInfo, error)
}
