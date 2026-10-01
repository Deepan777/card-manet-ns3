# SCR contrib module — fork of the ns-3.46.1 AODV module (src/aodv).
# Upstream src/aodv remains present and unmodified so that AODV-STOCK is a
# genuine native reference rather than a reconfigured fork.
build_lib(
  LIBNAME scr
  SOURCE_FILES
    helper/scr-helper.cc
    model/scr-dpd.cc
    model/scr-id-cache.cc
    model/scr-neighbor.cc
    model/scr-packet.cc
    model/scr-routing-protocol.cc
    model/scr-rqueue.cc
    model/scr-rtable.cc
    model/service-headers.cc
    model/deadline-telemetry-app.cc
    model/service-receipt-app.cc
    model/service-evidence.cc
    model/repair-ledger.cc
    model/origin-scheduler.cc
    model/no-discovery-tag.cc
    model/service-evidence-app.cc
  HEADER_FILES
    helper/scr-helper.h
    model/scr-dpd.h
    model/scr-id-cache.h
    model/scr-neighbor.h
    model/scr-packet.h
    model/scr-routing-protocol.h
    model/scr-rqueue.h
    model/scr-rtable.h
    model/service-headers.h
    model/deadline-telemetry-app.h
    model/service-receipt-app.h
    model/service-evidence.h
    model/repair-ledger.h
    model/origin-scheduler.h
    model/no-discovery-tag.h
    model/service-evidence-app.h
  LIBRARIES_TO_LINK
    ${libapplications}
    ${libinternet-apps}
    ${libwifi}
  TEST_SOURCES
    test/scr-service-headers-test-suite.cc
    test/scr-ledger-test-suite.cc
)
