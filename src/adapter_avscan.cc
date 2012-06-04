/*
 * Securepoint eCAP clamd Adapter
 * Copyright (C) 2011 Gernot Tenchio, Securepoint GmbH, Germany.
 *
 * http://www.securepoint.de/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * -----------------------------------------------------------------
 *
 * based on the eCAP adapter samples, see: http://www.e-cap.org/
 *
 * -----------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <limits.h>

#include <fstream>
#include <iostream>
#include <string>
#include <cerrno>

#include <libecap/common/message.h>
#include <libecap/common/errors.h>
#include <libecap/common/header.h>
#include <libecap/common/names.h>
#include <libecap/adapter/service.h>
#include <libecap/adapter/xaction.h>
#include <libecap/host/xaction.h>
#include <libecap/host/host.h>

#include "adapter_avscan_Service.h"
#include "adapter_avscan_Xaction.h"
#include "adapter_avscan.h"

using namespace std;

libecap::Area Adapter::Xaction::ErrorPage(void)
{
    std::string errmsg = "<html><head></head><body>";
    if (Ctx->status == stInfected) {
        errmsg += "<h1>Access denied!</h1>";
        errmsg += "You've tried to upload/download a file that contains the virus <b>";
        errmsg += "<b>" + statusString + "</b>.";
    } else {
        errmsg += "<h1>Internal error!</h1>";
        errmsg += "While scanning your request for virus infection an internal error occured!";
    }
    errmsg += "</body></html>\n";
    return libecap::Area::FromTempString(errmsg);
}

/**
 * Determines if we should scan or not.
  */
bool Adapter::Xaction::mustScan(libecap::Area area)
{
    FUNCENTER();
    if (bypass)
        return false;

    if (area.size && service->skipList->ready()) {
        const char *mimetype = magic_buffer(service->mcookie, area.start, area.size);
        if (mimetype) {
            if (service->skipList->match(mimetype))
                return false;
        }
    }
    return true;
}

void Adapter::Xaction::openTempfile(void)
{
    char fn[PATH_MAX];
    FUNCENTER();

    snprintf(fn, PATH_MAX - 1, "%s/squid-ecap-XXXXXX", service->tempdir.c_str());
    if (-1 == (Ctx->tempfd = mkstemp((char *)fn))) {
        ERR << "can't open temp file " << fn << endl;
        Ctx->status = stError;
        return;
    }
    unlink(fn);
}

int Adapter::Xaction::avWriteCommand(const char *command)
{
    fd_set wfds;
    struct timeval tv;
    int n;

    FUNCENTER();

    Must(command);
    n = strlen(command) + 1;

    tv.tv_sec = TIMEOUT;
    tv.tv_usec = 0;

    FD_ZERO(&wfds);
    FD_SET(Ctx->sockfd, &wfds);

    if (n == write(Ctx->sockfd, command, n)) {
        return n;
    } else if (n == -1 && errno != EAGAIN) {
        ERR << "write: " << strerror(errno) << endl;
    } else if (-1 == select(Ctx->sockfd + 1, &wfds, NULL, NULL, &tv)) {
        ERR << "select: " << strerror(errno) << endl;
    } else if (!(FD_ISSET(Ctx->sockfd, &wfds))) {
        ERR << "timeout @ " << Ctx->sockfd << endl;
    } else {
        // write the trailing NULL character too
        return write(Ctx->sockfd, command, n);
    }
    return -1;
}

int Adapter::Xaction::avReadResponse(void)
{
    char buf[1024];
    fd_set rfds;
    struct timeval tv;
    int n;

    FUNCENTER();

    tv.tv_sec = TIMEOUT;
    tv.tv_usec = 0;

    FD_ZERO(&rfds);
    FD_SET(Ctx->sockfd,&rfds);

    if (-1 != (n = read(Ctx->sockfd, buf, sizeof(buf)))) {
        /* looks good */
    } else if (errno != EAGAIN) {
        ERR << "read: " << strerror(errno) << endl;
    } else if (-1 == select(Ctx->sockfd + 1, &rfds, NULL, NULL, &tv)) {
        ERR << "select; " << strerror(errno) << endl;
    } else if (!FD_ISSET(Ctx->sockfd, &rfds)) {
        ERR << "timeout @ " << Ctx->sockfd << endl;
        return -2;
    } else if (-1 == (n = read(Ctx->sockfd, buf, sizeof(buf)))) {
        ERR << "read: " << strerror(errno) << endl;
    }

    if (n == -1) {
        /* */
    } else {
        if (n > 7) {
            char *colon = strrchr(buf, ':');
            char *eol = buf + n;
            if(!colon) {
                Ctx->status = stError;
            } else if(!memcmp(eol - 7, " FOUND", 6)) {
                Ctx->status = stInfected;
                statusString = ++colon;
                statusString.resize(statusString.size() - 6);
            } else if(!memcmp(eol - 7, " ERROR", 6)) {
                Ctx->status = stError;
            }
        }
        return n;
    }
    return -1;
}

static int doconnect(std::string aPath)
{
    int sockfd = -1;

    if ((sockfd = socket(AF_LOCAL, SOCK_STREAM, 0)) == -1) {
        ERR << "can't initialize clamd socket: " << strerror(errno) << endl;
    } else {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_LOCAL;
        strncpy(address.sun_path, aPath.c_str(), sizeof(address.sun_path));
        if (connect(sockfd, (struct sockaddr *) &address, sizeof(address)) == -1) {
            ERR << "can't connect to clamd socket: " << strerror(errno) << endl;
            close(sockfd);
            sockfd = -1;
        }
        fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK);
    }
    return sockfd;
}

void Adapter::Xaction::avStart(void)
{
    struct iovec iov[1];
    struct msghdr msg;
    struct cmsghdr *cmsg;
    unsigned char fdbuf[CMSG_SPACE(sizeof(int))];
    char dummy[]="";

    FUNCENTER();

    if (-1 == (Ctx->sockfd = doconnect(service->clamdsocket))) {
        Ctx->status = stError;
        return;
    }

    if (-1 == avWriteCommand("zFILDES")) {
        Ctx->status = stError;
        return;
    }

    iov[0].iov_base = dummy;
    iov[0].iov_len = 1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_control = fdbuf;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_controllen = CMSG_LEN(sizeof(int));
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    *(int *)CMSG_DATA(cmsg) = Ctx->tempfd;
    if(sendmsg(Ctx->sockfd, &msg, 0) == -1) {
        ERR << "FD send failed: " << strerror(errno) << endl;
        Ctx->status = stError;
    }
}

Adapter::Xaction::Xaction(libecap::shared_ptr < Service > aService, libecap::host::Xaction * x):service(aService), hostx(x),
    receivingVb(opUndecided),
    sendingAb(opUndecided)
{
    received = processed = 0;
    trickled = senderror = bypass = false;
}

Adapter::Xaction::~Xaction()
{
    FUNCENTER();

    if (Ctx) {
        if (-1 != Ctx->sockfd)
            close(Ctx->sockfd);

        if (-1 != Ctx->tempfd)
            close(Ctx->tempfd);

        free(Ctx);
    }

    if (libecap::host::Xaction * x = hostx) {
        hostx = 0;
        x->adaptationAborted();
    }
}

#ifndef V003
const libecap::Area Adapter::Xaction::option(const libecap::Name &) const {
    return libecap::Area(); // this transaction has no meta-information
}

void Adapter::Xaction::visitEachOption(libecap::NamedValueVisitor &) const {
    // this transaction has no meta-information to pass to the visitor
}
#endif

void Adapter::Xaction::start()
{
    FUNCENTER();
    Ctx = 0;

    Must(hostx);

    if (hostx->virgin().body()) {
        receivingVb = opOn;
        hostx->vbMake();            // ask host to supply virgin body
        Ctx = (struct Ctx *)calloc(1, sizeof(struct Ctx));
        Ctx->tempfd = Ctx->sockfd = -1;
        startTime = time(NULL);
    } else {
        hostx->useVirgin();
        receivingVb = opNever;
    }
}

void Adapter::Xaction::stop()
{
    FUNCENTER();
    hostx = 0;
    // the caller will delete
}

void Adapter::Xaction::abDiscard()
{
    FUNCENTER();

    Must(sendingAb == opUndecided);       // have not started yet
    sendingAb = opNever;
    stopVb();
}

void Adapter::Xaction::abMake()
{
    FUNCENTER();
    Must(sendingAb == opWaiting);       // have not yet started
    Must(hostx->virgin().body());	// that is our only source of ab content

    // we are or were receiving vb
    Must(receivingVb == opOn || receivingVb == opComplete);

    sendingAb = opOn;
}

void Adapter::Xaction::abMakeMore()
{
    FUNCENTER();
    Must(receivingVb == opOn);    // a precondition for receiving more vb
    hostx->vbMakeMore();
}

void Adapter::Xaction::abStopMaking()
{
    FUNCENTER();
    sendingAb = opComplete;
    stopVb();
}

libecap::Area Adapter::Xaction::abContent(UNUSED size_type offset, UNUSED size_type size)
{
    size_type sz;
    FUNCENTER();

    // required to not raise an exception on the final call with opComplete
    Must(sendingAb == opOn || sendingAb == opComplete);

    // Error?
    if (Ctx->status != stOK) {
        stopVb();
        sendingAb = opComplete;
        // Nothing written so far. We can send an error message!
        if (senderror)
            return ErrorPage();
    }

    // finished receiving?
    if (receivingVb == opComplete || bypass) {
        sz = sizeof(Ctx->buf); // use the whole buffer
        trickled = false;

        // finished sending?
        if (processed >= received) {
            sendingAb = opComplete;
            hostx->noteAbContentDone(true);
        }
    } else {
        sz = service->tricklesize;
    }

    // if complete, there is nothing more to return.
    if (sendingAb == opComplete || trickled) {
        trickled = false;
        return libecap::Area::FromTempString("");
    }

    lseek(Ctx->tempfd, processed, SEEK_SET);

    if (-1 == (sz = read(Ctx->tempfd, Ctx->buf,  sz))) {
        ERR << "can't read from temp file: " << strerror(errno) << endl;
        Ctx->status = stError;
        return libecap::Area::FromTempString("");
    }

    trickled = true;
    lastContent = time(NULL);
    return libecap::Area::FromTempBuffer(Ctx->buf, sz);
}

void Adapter::Xaction::abContentShift(size_type size)
{
    Must(sendingAb == opOn);
    processed += size;
}

void Adapter::Xaction::noteContentAvailable()
{
    FUNCENTER();

    if (sendingAb == opWaiting) {
        adapted = hostx->virgin().clone();
        Must(adapted != 0);

        libecap::FirstLine *firstLine = &(adapted->firstLine());
        libecap::StatusLine *statusLine = dynamic_cast<libecap::StatusLine*>(firstLine);

        // do not remove the Content-Length header in 'reqmod'
        if (statusLine)
            adapted->header().removeAny(libecap::headerContentLength);

        if (Ctx->status != stOK) {
            // last chance to indicate an error

            const libecap::Name name("Content-Type");
            const libecap::Name disp("Content-Disposition");
            const libecap::Header::Value value = libecap::Area::FromTempString("text/html");
            adapted->header().removeAny(disp);
            adapted->header().removeAny(name);
            adapted->header().add(name, value);

            if (statusLine)
                statusLine->statusCode(Ctx->status == stInfected ? 403 : 500);

            senderror = true;
        }

        const libecap::Name name("X-Ecap");
        const libecap::Header::Value value = libecap::Area::FromTempString(ADAPTERNAME);
        adapted->header().add(name, value);

        hostx->useAdapted(adapted);
    }
    hostx->noteAbContentAvailable();
}

// finished reading the virgin body
void Adapter::Xaction::noteVbContentDone(UNUSED bool atEnd)
{
    FUNCENTER();
    Must(Ctx);
    Must(receivingVb == opOn);

    receivingVb = opComplete;

    avStart();
    if (Ctx->status == stOK) {
        while (-2 == avReadResponse())
            ;
    }
    noteContentAvailable();
}

void Adapter::Xaction::processContent()
{
    time_t now = time(NULL);

    FUNCENTER();

    if (bypass) {
        noteContentAvailable();
    } else if (now < (startTime + service->trickletime)) {
        /* */
    } else if (now < (lastContent + service->trickletime)) {
        /* */
    } else {
        noteContentAvailable();
    }
}

void Adapter::Xaction::noteVbContentAvailable()
{
    FUNCENTER();
    Must(receivingVb == opOn);
    Must(Ctx);

    // get all virgin body
    const libecap::Area vb = hostx->vbContent(0, libecap::nsize);

    if (sendingAb == opUndecided) {

        // Try to read the ContentLength so we can decide whether scanning has
        // to be performed or not.
        if (service->maxscansize && hostx->virgin().header().hasAny(libecap::headerContentLength)) {
            const libecap::Header::Value value =
                hostx->virgin().header().value(libecap::headerContentLength);
            if (value.size > 0) {
                contentlength = strtoul(value.start, NULL, 10);
                if (contentlength > service->maxscansize)
                    bypass = 1;
                cerr << "Content-Length: " << value.start << " skip: " << (bypass ? "yes" : "no") << endl;
            }
        }

        if (mustScan(vb)) {
            openTempfile();
            // go to state waiting, hostx->useAdapted() will be called later
            // via noteContentAvailable()
            sendingAb = opWaiting;
        } else {
            // nothing to do, just send the vb
            hostx->useVirgin();
            abDiscard();
            return;
        }
    }

    Must(Ctx->tempfd != -1);
    lseek(Ctx->tempfd, 0, SEEK_END);

    // write body to temp file
    if (-1 == write(Ctx->tempfd, vb.start, vb.size)) {
        cerr << "can't write to temp file\n";
        Ctx->status = stError;
    }

    received += vb.size;

    // we have a copy; do not need vb any more
    hostx->vbContentShift(vb.size);

    // set bypass flag it we received more than maxscansize bytes
    if (service->maxscansize && received >= service->maxscansize)
        bypass = 1;

    if (sendingAb == opOn || sendingAb == opWaiting)
        processContent();
}

bool Adapter::Xaction::callable() const
{
    FUNCENTER();
    return hostx != 0;            // no point to call us if we are done
}

// tells the host that we are not interested in [more] vb
// if the host does not know that already
void Adapter::Xaction::stopVb()
{
    FUNCENTER();
    if (receivingVb == opOn) {
        hostx->vbStopMaking();
        receivingVb = opComplete;
    } else {
        // we already got the entire body or refused it earlier
        Must(receivingVb != opUndecided);
    }
}

// this method is used to make the last call to hostx transaction
// last call may delete adapter transaction if the host no longer needs it
libecap::host::Xaction * Adapter::Xaction::lastHostCall()
{
    FUNCENTER();
    libecap::host::Xaction * x = hostx;
    Must(x);
    hostx = 0;
    return x;
}
