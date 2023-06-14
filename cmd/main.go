package cmd

import (
	goflags "flag"

	"github.com/spf13/cobra"
	"k8s.innull.com/trafficlimitd/pkg/cgrouputils"
	"k8s.innull.com/trafficlimitd/pkg/clienthandler"
	"k8s.innull.com/trafficlimitd/pkg/server"
	"k8s.io/klog/v2"
)

var (
	mainCmd = &cobra.Command{
		Use:   "traffic-limitd [OPTIONS]",
		Short: "Daemon for limiting traffic",
		Long: `Daemon for limiting traffic
This application should be work with kubelet.`,
		Run: func(cmd *cobra.Command, args []string) {
			run()
		},
	}
	interfaces []string
	listenSock string
	maxTasks   int
)

// Execute executes the root command.
func Execute() error {
	return mainCmd.Execute()
}

func init() {
	cobra.OnInitialize(initConfig)

	fs := goflags.NewFlagSet("", goflags.PanicOnError)
	klog.InitFlags(fs)
	mainCmd.Flags().AddGoFlagSet(fs)

	mainCmd.Flags().StringSliceVarP(&interfaces, "interface", "i", []string{}, "Interface name")
	mainCmd.MarkFlagRequired("interface")
	mainCmd.Flags().StringVarP(&listenSock, "listen", "l", "/var/run/traffic-limitd.sock", "Listening socket")
	mainCmd.Flags().IntVarP(&maxTasks, "max-tasks", "m", 110, "Max tasks")
}

func initConfig() {
	maxTasks = maxTasks + maxTasks/5 + 5
}

func run() {
	_, err := cgrouputils.GetCgroupV2MountPoint()
	if err != nil {
		klog.Fatal(err)
	}
	ch := clienthandler.New()
	s := server.New(ch)
	if err := s.ListenAndServe(listenSock); err != nil {
		klog.Fatal(err)
	}
}
