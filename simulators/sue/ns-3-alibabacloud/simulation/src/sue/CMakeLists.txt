# SUE Scale-Up Ethernet module
# S0 scaffolding: compilable stubs for SueChannel / SueNetDevice / SUESwitchNode.
# Auto-discovered by top-level CMake (subdirlist src/).
build_lib(
  LIBNAME sue
  SOURCE_FILES
    model/sue-channel.cc
    model/sue-net-device.cc
    model/sue-switch-node.cc
    model/sue-header.cc
    model/sue-routing.cc
    model/sue-egress-queue.cc
    model/sue-cbfc-engine.cc
    model/sue-cbfc-counters.cc
    model/sue-llr-preamble-header.cc
    model/sue-error-model.cc
    model/sue-llr-engine.cc
    model/sue-llr-counters.cc
    model/sue-pfc-engine.cc
    model/sue-fc-controller.cc
    model/sue-ctl-header.cc
    model/sue-lldp-header.cc
    model/sue-llr-ctlos-header.cc
    model/sue-cbfc-header.cc
    model/sue-cbfc-cc-header.cc
    model/sue-pfc-header.cc
    model/sue-ctl-tag.cc
    model/sue-frame-bad-tag.cc
    network_frontend/sue-config.cc
    network_frontend/sue-entry.cc
    network_frontend/sue-lite-endpoint.cc
    network_frontend/sue-scale-topo.cc
  HEADER_FILES
    model/sue-channel.h
    model/sue-mtp.h
    model/sue-net-device.h
    model/sue-switch-node.h
    model/sue-header.h
    model/sue-shim-header.h
    model/sue-routing.h
    model/sue-egress-queue.h
    model/sue-cbfc-engine.h
    model/sue-cbfc-counters.h
    model/sue-llr-preamble-header.h
    model/sue-error-model.h
    model/sue-llr-engine.h
    model/sue-llr-counters.h
    model/sue-pfc-engine.h
    model/sue-fc-controller.h
    model/sue-ctl-header.h
    model/sue-lldp-header.h
    model/sue-llr-ctlos-header.h
    model/sue-cbfc-header.h
    model/sue-cbfc-cc-header.h
    model/sue-pfc-header.h
    model/sue-ctl-tag.h
    model/sue-frame-bad-tag.h
    network_frontend/sue-config.h
    network_frontend/sue-entry.h
    network_frontend/sue-lite-endpoint.h
    network_frontend/sue-scale-topo.h
  TEST_SOURCES test/sue-test.cc
  LIBRARIES_TO_LINK ${libnetwork}
                    ${libpoint-to-point}
)
