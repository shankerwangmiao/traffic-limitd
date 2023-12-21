package cmd

import (
	"context"
	"encoding/json"
	"errors"
	goflags "flag"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"
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
	authenticationv1 "k8s.io/api/authentication/v1"
	authenticationv1alpha1 "k8s.io/api/authentication/v1alpha1"
	authenticationv1beta1 "k8s.io/api/authentication/v1beta1"
	corev1 "k8s.io/api/core/v1"
	k8serrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	k8stypes "k8s.io/apimachinery/pkg/types"

	"k8s.innull.com/trafficlimitd/pkg/types"
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
	nodeName   string
	bps        string
	pps        string
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
	mainCmd.Flags().StringVar(&nodeName, "node-name", "", "This node name, must be same as kubelet's node name, can be infered from kubeconfig")
	mainCmd.Flags().StringVar(&bps, "bps", "", "Bandwidth limit in bytes per secon")
	mainCmd.Flags().StringVar(&pps, "pps", "", "Bandwidth limit in packets per secon")
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

	if nodeName == "" {
		klog.Info("Node name not specified, try to get node name using SelfSubjectReview API")
		name, err := getSelfNodeName(context.Background(), clientset)
		if err != nil {
			klog.Fatal(err)
		}
		nodeName = name
		klog.Infof("Got node name: %v", nodeName)
	} else {
		klog.Infof("Using node name %v", nodeName)
	}

	if bps == "" && pps == "" {
		klog.Warning("No bandwidth limit specified, will not declare available bandwidth to kubelet")
	} else if bps != "" && pps != "" {
		bpsVal, err := resource.ParseQuantity(bps)
		if err != nil {
			klog.Fatal("Invalid bandwidth limit specified:", err)
		}
		ppsVal, err := resource.ParseQuantity(pps)
		if err != nil {
			klog.Fatal("Invalid bandwidth limit specified:", err)
		}
		if bpsVal.Value() == 0 {
			klog.Fatal("Invalid bandwidth limit specified: 0Bps")
		}
		if ppsVal.Value() == 0 {
			klog.Fatal("Invalid bandwidth limit specified: 0pps")
		}
		klog.Infof("Will declare available bandwidth limit: %vBps, %vpps", bpsVal.Value(), ppsVal.Value())
		err = declareTrafficResource(context.Background(), clientset, nodeName, bpsVal, ppsVal)
		if err != nil {
			klog.Fatal("Error encountered when declaring available bandwidth limit:", err)
		}
		klog.Info("Declared available bandwidth limit")
	} else {
		klog.Fatal("Invalid bandwidth limit specified: either bps or pps is not specified")
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

	go func() {
		//Signal Handler
		c := make(chan os.Signal, 10)
		signal.Notify(c, syscall.SIGINT, syscall.SIGTERM)

		for sig := range c {
			klog.Infof("Received signal: %v, exiting...", sig)
			go s.Close()
		}

	}()

	if err := s.ListenAndServe(listenSock); err != nil {
		klog.Fatal(err)
	}
	klog.Info("Server stopped, exiting...")
}

func getSelfNodeName(ctx context.Context, clientset *kubernetes.Clientset) (string, error) {
	var (
		res runtime.Object
		err error
	)
	res, err = clientset.AuthenticationV1().SelfSubjectReviews().Create(ctx, &authenticationv1.SelfSubjectReview{}, metav1.CreateOptions{})
	if err != nil && k8serrors.IsNotFound(err) {
		res, err = clientset.AuthenticationV1beta1().SelfSubjectReviews().Create(ctx, &authenticationv1beta1.SelfSubjectReview{}, metav1.CreateOptions{})
		if err != nil && k8serrors.IsNotFound(err) {
			res, err = clientset.AuthenticationV1alpha1().SelfSubjectReviews().Create(ctx, &authenticationv1alpha1.SelfSubjectReview{}, metav1.CreateOptions{})
		}
	}
	if err != nil {
		if k8serrors.IsForbidden(err) {
			return "", fmt.Errorf("Unable to detect node name: Forbidden to create SelfSubjectReview, please check RBAC")
		} else if k8serrors.IsNotFound(err) {
			return "", fmt.Errorf("Unable to detect node name: No SelfSubjectReview API found")
		} else {
			return "", fmt.Errorf("Unable to detect node name: %v", err)
		}
	}
	var username string
	switch res.(type) {
	case *authenticationv1.SelfSubjectReview:
		username = res.(*authenticationv1.SelfSubjectReview).Status.UserInfo.Username
	case *authenticationv1beta1.SelfSubjectReview:
		username = res.(*authenticationv1beta1.SelfSubjectReview).Status.UserInfo.Username
	case *authenticationv1alpha1.SelfSubjectReview:
		username = res.(*authenticationv1alpha1.SelfSubjectReview).Status.UserInfo.Username
	default:
		return "", fmt.Errorf("Unable to detect node name: SelfSubjectReview returned unexpected response type %T", res)
	}
	if username == "" {
		return "", fmt.Errorf("Unable to detect node name: SelfSubjectReview returned empty username")
	}
	if strings.HasPrefix(username, "system:node:") {
		return strings.TrimPrefix(username, "system:node:"), nil
	} else {
		return "", fmt.Errorf("Unable to detect node name: SelfSubjectReview returned unexpected username %s, expected name with prefix system:node:", username)
	}
}

func declareTrafficResource(ctx context.Context, clientset *kubernetes.Clientset, nodeName string, bps, pps resource.Quantity) error {

	var patch []map[string]interface{}
	rfc6901Encoder := strings.NewReplacer("~", "~0", "/", "~1")
	patch = append(patch, map[string]interface{}{
		"op":    "add",
		"path":  "/status/capacity/" + rfc6901Encoder.Replace(types.BytesPerSecondResourceName),
		"value": bps.String(),
	})
	patch = append(patch, map[string]interface{}{
		"op":    "add",
		"path":  "/status/capacity/" + rfc6901Encoder.Replace(types.PacketsPerSecondResourceName),
		"value": pps.String(),
	})

	patchBytes, err := json.Marshal(patch)

	if err != nil {
		return err
	}

	err = clientset.CoreV1().RESTClient().Patch(k8stypes.JSONPatchType).
		Resource("nodes").
		Name(nodeName).
		SubResource("status").
		Body(patchBytes).
		Do(ctx).
		Into(&corev1.Node{})

	if err != nil {
		return err
	}

	return nil
}
