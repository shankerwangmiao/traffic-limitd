package cmd

import (
	"errors"
	goflags "flag"
	"os"
	"time"

	"github.com/spf13/cobra"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"
	"k8s.io/klog/v2"

	"k8s.innull.com/trafficlimitd/pkg/bpftrafficlimiter"
	"k8s.innull.com/trafficlimitd/pkg/cgrouputils"
	"k8s.innull.com/trafficlimitd/pkg/clienthandler"
	"k8s.innull.com/trafficlimitd/pkg/k8sclient"
	"k8s.innull.com/trafficlimitd/pkg/server"
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
	kubeconfig string
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
	mainCmd.Flags().StringVar(&kubeconfig, "kubeconfig", "", "Path to kubeconfig file with authorization and master location information.")
	mainCmd.MarkFlagRequired("kubeconfig")
}

func initConfig() {
}

func run() {
	_, err := cgrouputils.GetCgroupV2MountPoint()
	if err != nil {
		klog.Fatal(err)
	}

	for {
		_, err = os.Stat(kubeconfig)
		if errors.Is(err, os.ErrNotExist) {
			klog.Info("Wait 1s for kubelet to create kubeconfig...")
			<-time.After(1 * time.Second)
		} else if err != nil {
			klog.Fatal("Cannot Stat", kubeconfig, err)
		} else {
			break
		}
	}

	config, err := clientcmd.BuildConfigFromFlags("", kubeconfig)
	if err != nil {
		klog.Fatal(err)
	}
	clientset, err := kubernetes.NewForConfig(config)
	if err != nil {
		klog.Fatal(err)
	}

	fetcher := k8sclient.NewK8sTrafficLimitInfoFetcher(clientset)
	defer fetcher.Close()
	limiter := &bpftrafficlimiter.Limiter
	err = limiter.LoadBPF(maxTasks)
	if err != nil {
		klog.Fatal("Error encountered when loading ebpf program:", err)
	}
	defer limiter.Close()
	err = limiter.ArmOnInterfaces(interfaces)
	if err != nil {
		klog.Fatal("Error encountered when setting up interfaces", err)
	}

	clihandler := clienthandler.New(fetcher, limiter)
	s := server.New(clihandler)

	if err := s.ListenAndServe(listenSock); err != nil {
		klog.Fatal(err)
	}
	klog.Info("Server stopped, exiting...")
}
