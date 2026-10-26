set(mpi_sources)
set(mpi_headers)
set(mpi_libraries)

# NS3_MTP build fix: with the global -DNS3_MTP define, rdma-hw.cc references
# MtpInterface symbols (src/mtp) but this module does not declare the
# dependency. lib<name> vars are pre-defined by filter_enabled_and_disabled
# modules, so ${libmtp} is valid here. Linking it makes libpoint-to-point.so
# resolve MtpInterface::g_e_inCriticalSection at its own build; otherwise
# downstream executables (scratch) fail with an undefined-reference to it
# (--as-needed drops libmtp, which precedes point-to-point on the link line).
if(${NS3_MTP})
  set(mtp_libraries ${libmtp})
endif()

if(${ENABLE_MPI})
  set(mpi_sources
      # model/point-to-point-remote-channel.cc
  )
  set(mpi_headers
      # model/point-to-point-remote-channel.h
  )
  set(mpi_libraries
      # ${libmpi}
      # ${MPI_CXX_LIBRARIES}
  )
endif()

build_lib(
  LIBNAME point-to-point
  SOURCE_FILES
    ${mpi_sources}
    helper/point-to-point-helper.cc
    helper/qbb-helper.cc
    model/point-to-point-channel.cc
    model/point-to-point-net-device.cc
    model/ppp-header.cc
    model/qbb-net-device.cc
    model/pause-header.cc
    model/cn-header.cc
    model/pint.cc
    model/qbb-header.cc
    model/qbb-channel.cc
    model/qbb-remote-channel.cc
    model/rdma-driver.cc
    model/rdma-queue-pair.cc
    model/rdma-hw.cc
    model/switch-node.cc
    model/switch-mmu.cc
    model/nvswitch-node.cc
    model/point-to-point-remote-channel.cc
  HEADER_FILES
    ${mpi_headers}
    helper/point-to-point-helper.h
    helper/qbb-helper.h
    helper/sim-setting.h
    model/point-to-point-channel.h
    model/point-to-point-net-device.h
    model/ppp-header.h
    model/qbb-net-device.h
    model/pause-header.h
    model/cn-header.h
    model/pint.h
    model/qbb-header.h
    model/qbb-channel.h
    model/qbb-remote-channel.h
    model/rdma-driver.h
    model/rdma-queue-pair.h
    model/rdma-hw.h
    model/switch-node.h
    model/switch-mmu.h
    model/trace-format.h
    model/nvswitch-node.h
    model/point-to-point-remote-channel.h
  LIBRARIES_TO_LINK ${libnetwork}
                    ${libinternet}
                    ${libapplications}
                    ${mtp_libraries}
                    ${mpi_libraries}
  TEST_SOURCES test/point-to-point-test.cc
)
