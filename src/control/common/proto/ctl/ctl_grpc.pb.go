// Code generated by protoc-gen-go-grpc. DO NOT EDIT.
// versions:
// - protoc-gen-go-grpc v1.3.0
// - protoc             v3.5.0
// source: ctl/ctl.proto

package ctl

import (
	context "context"
	grpc "google.golang.org/grpc"
	codes "google.golang.org/grpc/codes"
	status "google.golang.org/grpc/status"
)

// This is a compile-time assertion to ensure that this generated file
// is compatible with the grpc package it is being compiled against.
// Requires gRPC-Go v1.32.0 or later.
const _ = grpc.SupportPackageIsVersion7

const (
	CtlSvc_StorageScan_FullMethodName          = "/ctl.CtlSvc/StorageScan"
	CtlSvc_StorageFormat_FullMethodName        = "/ctl.CtlSvc/StorageFormat"
	CtlSvc_StorageNvmeRebind_FullMethodName    = "/ctl.CtlSvc/StorageNvmeRebind"
	CtlSvc_StorageNvmeAddDevice_FullMethodName = "/ctl.CtlSvc/StorageNvmeAddDevice"
	CtlSvc_NetworkScan_FullMethodName          = "/ctl.CtlSvc/NetworkScan"
	CtlSvc_FirmwareQuery_FullMethodName        = "/ctl.CtlSvc/FirmwareQuery"
	CtlSvc_FirmwareUpdate_FullMethodName       = "/ctl.CtlSvc/FirmwareUpdate"
	CtlSvc_SmdQuery_FullMethodName             = "/ctl.CtlSvc/SmdQuery"
	CtlSvc_SmdManage_FullMethodName            = "/ctl.CtlSvc/SmdManage"
	CtlSvc_SetEngineLogMasks_FullMethodName    = "/ctl.CtlSvc/SetEngineLogMasks"
	CtlSvc_PrepShutdownRanks_FullMethodName    = "/ctl.CtlSvc/PrepShutdownRanks"
	CtlSvc_StopRanks_FullMethodName            = "/ctl.CtlSvc/StopRanks"
	CtlSvc_ResetFormatRanks_FullMethodName     = "/ctl.CtlSvc/ResetFormatRanks"
	CtlSvc_StartRanks_FullMethodName           = "/ctl.CtlSvc/StartRanks"
	CtlSvc_CollectLog_FullMethodName           = "/ctl.CtlSvc/CollectLog"
)

// CtlSvcClient is the client API for CtlSvc service.
//
// For semantics around ctx use and closing/ending streaming RPCs, please refer to https://pkg.go.dev/google.golang.org/grpc/?tab=doc#ClientConn.NewStream.
type CtlSvcClient interface {
	// Retrieve details of nonvolatile storage on server, including health info
	StorageScan(ctx context.Context, in *StorageScanReq, opts ...grpc.CallOption) (*StorageScanResp, error)
	// Format nonvolatile storage devices for use with DAOS
	StorageFormat(ctx context.Context, in *StorageFormatReq, opts ...grpc.CallOption) (*StorageFormatResp, error)
	// Rebind SSD from kernel and bind instead to user-space for use with DAOS
	StorageNvmeRebind(ctx context.Context, in *NvmeRebindReq, opts ...grpc.CallOption) (*NvmeRebindResp, error)
	// Add newly inserted SSD to DAOS engine config
	StorageNvmeAddDevice(ctx context.Context, in *NvmeAddDeviceReq, opts ...grpc.CallOption) (*NvmeAddDeviceResp, error)
	// Perform a fabric scan to determine the available provider, device, NUMA node combinations
	NetworkScan(ctx context.Context, in *NetworkScanReq, opts ...grpc.CallOption) (*NetworkScanResp, error)
	// Retrieve firmware details from storage devices on server
	FirmwareQuery(ctx context.Context, in *FirmwareQueryReq, opts ...grpc.CallOption) (*FirmwareQueryResp, error)
	// Update firmware on storage devices on server
	FirmwareUpdate(ctx context.Context, in *FirmwareUpdateReq, opts ...grpc.CallOption) (*FirmwareUpdateResp, error)
	// Query the per-server metadata
	SmdQuery(ctx context.Context, in *SmdQueryReq, opts ...grpc.CallOption) (*SmdQueryResp, error)
	// Manage devices (per-server) identified in SMD table
	SmdManage(ctx context.Context, in *SmdManageReq, opts ...grpc.CallOption) (*SmdManageResp, error)
	// Set log level for DAOS I/O Engines on a host.
	SetEngineLogMasks(ctx context.Context, in *SetLogMasksReq, opts ...grpc.CallOption) (*SetLogMasksResp, error)
	// Prepare DAOS I/O Engines on a host for controlled shutdown. (gRPC fanout)
	PrepShutdownRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error)
	// Stop DAOS I/O Engines on a host. (gRPC fanout)
	StopRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error)
	// ResetFormat DAOS I/O Engines on a host. (gRPC fanout)
	ResetFormatRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error)
	// Start DAOS I/O Engines on a host. (gRPC fanout)
	StartRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error)
	// Perform a Log collection on Servers for support/debug purpose
	CollectLog(ctx context.Context, in *CollectLogReq, opts ...grpc.CallOption) (*CollectLogResp, error)
}

type ctlSvcClient struct {
	cc grpc.ClientConnInterface
}

func NewCtlSvcClient(cc grpc.ClientConnInterface) CtlSvcClient {
	return &ctlSvcClient{cc}
}

func (c *ctlSvcClient) StorageScan(ctx context.Context, in *StorageScanReq, opts ...grpc.CallOption) (*StorageScanResp, error) {
	out := new(StorageScanResp)
	err := c.cc.Invoke(ctx, CtlSvc_StorageScan_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) StorageFormat(ctx context.Context, in *StorageFormatReq, opts ...grpc.CallOption) (*StorageFormatResp, error) {
	out := new(StorageFormatResp)
	err := c.cc.Invoke(ctx, CtlSvc_StorageFormat_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) StorageNvmeRebind(ctx context.Context, in *NvmeRebindReq, opts ...grpc.CallOption) (*NvmeRebindResp, error) {
	out := new(NvmeRebindResp)
	err := c.cc.Invoke(ctx, CtlSvc_StorageNvmeRebind_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) StorageNvmeAddDevice(ctx context.Context, in *NvmeAddDeviceReq, opts ...grpc.CallOption) (*NvmeAddDeviceResp, error) {
	out := new(NvmeAddDeviceResp)
	err := c.cc.Invoke(ctx, CtlSvc_StorageNvmeAddDevice_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) NetworkScan(ctx context.Context, in *NetworkScanReq, opts ...grpc.CallOption) (*NetworkScanResp, error) {
	out := new(NetworkScanResp)
	err := c.cc.Invoke(ctx, CtlSvc_NetworkScan_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) FirmwareQuery(ctx context.Context, in *FirmwareQueryReq, opts ...grpc.CallOption) (*FirmwareQueryResp, error) {
	out := new(FirmwareQueryResp)
	err := c.cc.Invoke(ctx, CtlSvc_FirmwareQuery_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) FirmwareUpdate(ctx context.Context, in *FirmwareUpdateReq, opts ...grpc.CallOption) (*FirmwareUpdateResp, error) {
	out := new(FirmwareUpdateResp)
	err := c.cc.Invoke(ctx, CtlSvc_FirmwareUpdate_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) SmdQuery(ctx context.Context, in *SmdQueryReq, opts ...grpc.CallOption) (*SmdQueryResp, error) {
	out := new(SmdQueryResp)
	err := c.cc.Invoke(ctx, CtlSvc_SmdQuery_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) SmdManage(ctx context.Context, in *SmdManageReq, opts ...grpc.CallOption) (*SmdManageResp, error) {
	out := new(SmdManageResp)
	err := c.cc.Invoke(ctx, CtlSvc_SmdManage_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) SetEngineLogMasks(ctx context.Context, in *SetLogMasksReq, opts ...grpc.CallOption) (*SetLogMasksResp, error) {
	out := new(SetLogMasksResp)
	err := c.cc.Invoke(ctx, CtlSvc_SetEngineLogMasks_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) PrepShutdownRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error) {
	out := new(RanksResp)
	err := c.cc.Invoke(ctx, CtlSvc_PrepShutdownRanks_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) StopRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error) {
	out := new(RanksResp)
	err := c.cc.Invoke(ctx, CtlSvc_StopRanks_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) ResetFormatRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error) {
	out := new(RanksResp)
	err := c.cc.Invoke(ctx, CtlSvc_ResetFormatRanks_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) StartRanks(ctx context.Context, in *RanksReq, opts ...grpc.CallOption) (*RanksResp, error) {
	out := new(RanksResp)
	err := c.cc.Invoke(ctx, CtlSvc_StartRanks_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

func (c *ctlSvcClient) CollectLog(ctx context.Context, in *CollectLogReq, opts ...grpc.CallOption) (*CollectLogResp, error) {
	out := new(CollectLogResp)
	err := c.cc.Invoke(ctx, CtlSvc_CollectLog_FullMethodName, in, out, opts...)
	if err != nil {
		return nil, err
	}
	return out, nil
}

// CtlSvcServer is the server API for CtlSvc service.
// All implementations must embed UnimplementedCtlSvcServer
// for forward compatibility
type CtlSvcServer interface {
	// Retrieve details of nonvolatile storage on server, including health info
	StorageScan(context.Context, *StorageScanReq) (*StorageScanResp, error)
	// Format nonvolatile storage devices for use with DAOS
	StorageFormat(context.Context, *StorageFormatReq) (*StorageFormatResp, error)
	// Rebind SSD from kernel and bind instead to user-space for use with DAOS
	StorageNvmeRebind(context.Context, *NvmeRebindReq) (*NvmeRebindResp, error)
	// Add newly inserted SSD to DAOS engine config
	StorageNvmeAddDevice(context.Context, *NvmeAddDeviceReq) (*NvmeAddDeviceResp, error)
	// Perform a fabric scan to determine the available provider, device, NUMA node combinations
	NetworkScan(context.Context, *NetworkScanReq) (*NetworkScanResp, error)
	// Retrieve firmware details from storage devices on server
	FirmwareQuery(context.Context, *FirmwareQueryReq) (*FirmwareQueryResp, error)
	// Update firmware on storage devices on server
	FirmwareUpdate(context.Context, *FirmwareUpdateReq) (*FirmwareUpdateResp, error)
	// Query the per-server metadata
	SmdQuery(context.Context, *SmdQueryReq) (*SmdQueryResp, error)
	// Manage devices (per-server) identified in SMD table
	SmdManage(context.Context, *SmdManageReq) (*SmdManageResp, error)
	// Set log level for DAOS I/O Engines on a host.
	SetEngineLogMasks(context.Context, *SetLogMasksReq) (*SetLogMasksResp, error)
	// Prepare DAOS I/O Engines on a host for controlled shutdown. (gRPC fanout)
	PrepShutdownRanks(context.Context, *RanksReq) (*RanksResp, error)
	// Stop DAOS I/O Engines on a host. (gRPC fanout)
	StopRanks(context.Context, *RanksReq) (*RanksResp, error)
	// ResetFormat DAOS I/O Engines on a host. (gRPC fanout)
	ResetFormatRanks(context.Context, *RanksReq) (*RanksResp, error)
	// Start DAOS I/O Engines on a host. (gRPC fanout)
	StartRanks(context.Context, *RanksReq) (*RanksResp, error)
	// Perform a Log collection on Servers for support/debug purpose
	CollectLog(context.Context, *CollectLogReq) (*CollectLogResp, error)
	mustEmbedUnimplementedCtlSvcServer()
}

// UnimplementedCtlSvcServer must be embedded to have forward compatible implementations.
type UnimplementedCtlSvcServer struct {
}

func (UnimplementedCtlSvcServer) StorageScan(context.Context, *StorageScanReq) (*StorageScanResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method StorageScan not implemented")
}
func (UnimplementedCtlSvcServer) StorageFormat(context.Context, *StorageFormatReq) (*StorageFormatResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method StorageFormat not implemented")
}
func (UnimplementedCtlSvcServer) StorageNvmeRebind(context.Context, *NvmeRebindReq) (*NvmeRebindResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method StorageNvmeRebind not implemented")
}
func (UnimplementedCtlSvcServer) StorageNvmeAddDevice(context.Context, *NvmeAddDeviceReq) (*NvmeAddDeviceResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method StorageNvmeAddDevice not implemented")
}
func (UnimplementedCtlSvcServer) NetworkScan(context.Context, *NetworkScanReq) (*NetworkScanResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method NetworkScan not implemented")
}
func (UnimplementedCtlSvcServer) FirmwareQuery(context.Context, *FirmwareQueryReq) (*FirmwareQueryResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method FirmwareQuery not implemented")
}
func (UnimplementedCtlSvcServer) FirmwareUpdate(context.Context, *FirmwareUpdateReq) (*FirmwareUpdateResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method FirmwareUpdate not implemented")
}
func (UnimplementedCtlSvcServer) SmdQuery(context.Context, *SmdQueryReq) (*SmdQueryResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method SmdQuery not implemented")
}
func (UnimplementedCtlSvcServer) SmdManage(context.Context, *SmdManageReq) (*SmdManageResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method SmdManage not implemented")
}
func (UnimplementedCtlSvcServer) SetEngineLogMasks(context.Context, *SetLogMasksReq) (*SetLogMasksResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method SetEngineLogMasks not implemented")
}
func (UnimplementedCtlSvcServer) PrepShutdownRanks(context.Context, *RanksReq) (*RanksResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method PrepShutdownRanks not implemented")
}
func (UnimplementedCtlSvcServer) StopRanks(context.Context, *RanksReq) (*RanksResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method StopRanks not implemented")
}
func (UnimplementedCtlSvcServer) ResetFormatRanks(context.Context, *RanksReq) (*RanksResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method ResetFormatRanks not implemented")
}
func (UnimplementedCtlSvcServer) StartRanks(context.Context, *RanksReq) (*RanksResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method StartRanks not implemented")
}
func (UnimplementedCtlSvcServer) CollectLog(context.Context, *CollectLogReq) (*CollectLogResp, error) {
	return nil, status.Errorf(codes.Unimplemented, "method CollectLog not implemented")
}
func (UnimplementedCtlSvcServer) mustEmbedUnimplementedCtlSvcServer() {}

// UnsafeCtlSvcServer may be embedded to opt out of forward compatibility for this service.
// Use of this interface is not recommended, as added methods to CtlSvcServer will
// result in compilation errors.
type UnsafeCtlSvcServer interface {
	mustEmbedUnimplementedCtlSvcServer()
}

func RegisterCtlSvcServer(s grpc.ServiceRegistrar, srv CtlSvcServer) {
	s.RegisterService(&CtlSvc_ServiceDesc, srv)
}

func _CtlSvc_StorageScan_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(StorageScanReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).StorageScan(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_StorageScan_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).StorageScan(ctx, req.(*StorageScanReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_StorageFormat_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(StorageFormatReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).StorageFormat(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_StorageFormat_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).StorageFormat(ctx, req.(*StorageFormatReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_StorageNvmeRebind_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(NvmeRebindReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).StorageNvmeRebind(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_StorageNvmeRebind_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).StorageNvmeRebind(ctx, req.(*NvmeRebindReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_StorageNvmeAddDevice_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(NvmeAddDeviceReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).StorageNvmeAddDevice(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_StorageNvmeAddDevice_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).StorageNvmeAddDevice(ctx, req.(*NvmeAddDeviceReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_NetworkScan_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(NetworkScanReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).NetworkScan(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_NetworkScan_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).NetworkScan(ctx, req.(*NetworkScanReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_FirmwareQuery_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(FirmwareQueryReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).FirmwareQuery(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_FirmwareQuery_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).FirmwareQuery(ctx, req.(*FirmwareQueryReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_FirmwareUpdate_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(FirmwareUpdateReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).FirmwareUpdate(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_FirmwareUpdate_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).FirmwareUpdate(ctx, req.(*FirmwareUpdateReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_SmdQuery_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(SmdQueryReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).SmdQuery(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_SmdQuery_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).SmdQuery(ctx, req.(*SmdQueryReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_SmdManage_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(SmdManageReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).SmdManage(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_SmdManage_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).SmdManage(ctx, req.(*SmdManageReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_SetEngineLogMasks_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(SetLogMasksReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).SetEngineLogMasks(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_SetEngineLogMasks_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).SetEngineLogMasks(ctx, req.(*SetLogMasksReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_PrepShutdownRanks_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(RanksReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).PrepShutdownRanks(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_PrepShutdownRanks_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).PrepShutdownRanks(ctx, req.(*RanksReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_StopRanks_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(RanksReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).StopRanks(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_StopRanks_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).StopRanks(ctx, req.(*RanksReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_ResetFormatRanks_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(RanksReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).ResetFormatRanks(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_ResetFormatRanks_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).ResetFormatRanks(ctx, req.(*RanksReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_StartRanks_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(RanksReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).StartRanks(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_StartRanks_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).StartRanks(ctx, req.(*RanksReq))
	}
	return interceptor(ctx, in, info, handler)
}

func _CtlSvc_CollectLog_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(CollectLogReq)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(CtlSvcServer).CollectLog(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: CtlSvc_CollectLog_FullMethodName,
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(CtlSvcServer).CollectLog(ctx, req.(*CollectLogReq))
	}
	return interceptor(ctx, in, info, handler)
}

// CtlSvc_ServiceDesc is the grpc.ServiceDesc for CtlSvc service.
// It's only intended for direct use with grpc.RegisterService,
// and not to be introspected or modified (even as a copy)
var CtlSvc_ServiceDesc = grpc.ServiceDesc{
	ServiceName: "ctl.CtlSvc",
	HandlerType: (*CtlSvcServer)(nil),
	Methods: []grpc.MethodDesc{
		{
			MethodName: "StorageScan",
			Handler:    _CtlSvc_StorageScan_Handler,
		},
		{
			MethodName: "StorageFormat",
			Handler:    _CtlSvc_StorageFormat_Handler,
		},
		{
			MethodName: "StorageNvmeRebind",
			Handler:    _CtlSvc_StorageNvmeRebind_Handler,
		},
		{
			MethodName: "StorageNvmeAddDevice",
			Handler:    _CtlSvc_StorageNvmeAddDevice_Handler,
		},
		{
			MethodName: "NetworkScan",
			Handler:    _CtlSvc_NetworkScan_Handler,
		},
		{
			MethodName: "FirmwareQuery",
			Handler:    _CtlSvc_FirmwareQuery_Handler,
		},
		{
			MethodName: "FirmwareUpdate",
			Handler:    _CtlSvc_FirmwareUpdate_Handler,
		},
		{
			MethodName: "SmdQuery",
			Handler:    _CtlSvc_SmdQuery_Handler,
		},
		{
			MethodName: "SmdManage",
			Handler:    _CtlSvc_SmdManage_Handler,
		},
		{
			MethodName: "SetEngineLogMasks",
			Handler:    _CtlSvc_SetEngineLogMasks_Handler,
		},
		{
			MethodName: "PrepShutdownRanks",
			Handler:    _CtlSvc_PrepShutdownRanks_Handler,
		},
		{
			MethodName: "StopRanks",
			Handler:    _CtlSvc_StopRanks_Handler,
		},
		{
			MethodName: "ResetFormatRanks",
			Handler:    _CtlSvc_ResetFormatRanks_Handler,
		},
		{
			MethodName: "StartRanks",
			Handler:    _CtlSvc_StartRanks_Handler,
		},
		{
			MethodName: "CollectLog",
			Handler:    _CtlSvc_CollectLog_Handler,
		},
	},
	Streams:  []grpc.StreamDesc{},
	Metadata: "ctl/ctl.proto",
}
