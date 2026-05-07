#
# cody DOS Makefile — Open Watcom 1.9
# Run from the DOS/ directory after sourcing watcomenv.sh:
#   . ./watcomenv.sh && wmake
#
# Compile options live in cody.rsp (and trace.rsp for trace.cpp).
# This avoids wmake quote-stripping issues with -DCFG_H="cody.cfg".
#

tcp_c  = mTCP/tcplib
src    = ../src

all : cody.exe

clean : .symbolic
	rm -f cody.exe *.obj *.map

# -----------------------------------------------------------------------
# mTCP library objects
# -----------------------------------------------------------------------

packet.obj : $(tcp_c)/packet.cpp
	wpp $(tcp_c)/packet.cpp @cody.rsp -fo=$@

arp.obj : $(tcp_c)/arp.cpp
	wpp $(tcp_c)/arp.cpp @cody.rsp -fo=$@

eth.obj : $(tcp_c)/eth.cpp
	wpp $(tcp_c)/eth.cpp @cody.rsp -fo=$@

ip.obj : $(tcp_c)/ip.cpp
	wpp $(tcp_c)/ip.cpp @cody.rsp -fo=$@

tcp.obj : $(tcp_c)/tcp.cpp
	wpp $(tcp_c)/tcp.cpp @cody.rsp -fo=$@

tcpsockm.obj : $(tcp_c)/tcpsockm.cpp
	wpp $(tcp_c)/tcpsockm.cpp @cody.rsp -fo=$@

udp.obj : $(tcp_c)/udp.cpp
	wpp $(tcp_c)/udp.cpp @cody.rsp -fo=$@

utils.obj : $(tcp_c)/utils.cpp
	wpp $(tcp_c)/utils.cpp @cody.rsp -fo=$@

dns.obj : $(tcp_c)/dns.cpp
	wpp $(tcp_c)/dns.cpp @cody.rsp -fo=$@

timer.obj : $(tcp_c)/timer.cpp
	wpp $(tcp_c)/timer.cpp @cody.rsp -fo=$@

trace.obj : $(tcp_c)/trace.cpp
	wpp $(tcp_c)/trace.cpp @trace.rsp -fo=$@

ipasm.obj : $(tcp_c)/ipasm.asm
	wasm -0 -ml $(tcp_c)/ipasm.asm -fo=$@

# -----------------------------------------------------------------------
# Application objects
# -----------------------------------------------------------------------

cody.obj : $(src)/cody.cpp
	wpp $(src)/cody.cpp @cody.rsp -fo=$@

http.obj : $(src)/http.cpp
	wpp $(src)/http.cpp @cody.rsp -fo=$@

json.obj : $(src)/json.cpp
	wpp $(src)/json.cpp @cody.rsp -fo=$@

session.obj : $(src)/session.cpp
	wpp $(src)/session.cpp @cody.rsp -fo=$@

tools.obj : $(src)/tools.cpp
	wpp $(src)/tools.cpp @cody.rsp -fo=$@

socket_dos.obj : socket_dos.cpp
	wpp socket_dos.cpp @cody.rsp -fo=$@

console_dos.obj : console_dos.cpp
	wpp console_dos.cpp @cody.rsp -fo=$@

tools_dos.obj : tools_dos.cpp
	wpp tools_dos.cpp @cody.rsp -fo=$@

tui_dos.obj : tui_dos.cpp
	wpp tui_dos.cpp @cody.rsp -fo=$@

ui.obj : $(src)/ui.cpp
	wpp $(src)/ui.cpp @cody.rsp -fo=$@

log.obj : $(src)/log.cpp
	wpp $(src)/log.cpp @cody.rsp -fo=$@

# -----------------------------------------------------------------------
# Link
# -----------------------------------------------------------------------

tcpobjs = packet.obj arp.obj eth.obj ip.obj tcp.obj tcpsockm.obj &
          udp.obj utils.obj dns.obj timer.obj ipasm.obj trace.obj

appobjs = cody.obj http.obj json.obj session.obj tools.obj ui.obj log.obj &
          console_dos.obj socket_dos.obj tools_dos.obj tui_dos.obj

cody.exe : $(tcpobjs) $(appobjs)
	wlink system dos option map option eliminate option stack=4096 name cody.exe file { $(tcpobjs) $(appobjs) }
