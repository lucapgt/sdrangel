///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2018 Edouard Griffiths, F4EXB                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <algorithm>

#include "dsp/samplesourcefifo.h"

#include "bladerf2outputthread.h"

BladeRF2OutputThread::BladeRF2OutputThread(struct bladerf* dev, unsigned int nbTxChannels, QObject* parent) :
    QThread(parent),
    m_running(false),
    m_dev(dev),
    m_nbChannels(nbTxChannels)
{
    qDebug("BladeRF2OutputThread::BladeRF2OutputThread");
    m_channels = new Channel[nbTxChannels];
    m_buf = new qint16[2*DeviceBladeRF2::blockSize*nbTxChannels];
}

BladeRF2OutputThread::~BladeRF2OutputThread()
{
    qDebug("BladeRF2OutputThread::~BladeRF2OutputThread");

    if (m_running) {
        stopWork();
    }

    delete[] m_buf;
    delete[] m_channels;
}

void BladeRF2OutputThread::startWork()
{
    m_startWaitMutex.lock();
    start();

    while(!m_running) {
        m_startWaiter.wait(&m_startWaitMutex, 100);
    }

    m_startWaitMutex.unlock();
}

void BladeRF2OutputThread::stopWork()
{
    m_running = false;
    wait();
}

void BladeRF2OutputThread::run()
{
    int res;

    m_running = true;
    m_startWaiter.wakeAll();

    unsigned int nbFifos = getNbFifos();

    if ((m_nbChannels > 0) && (nbFifos > 0))
    {
        int status;

        if (m_nbChannels > 1) {
            status = bladerf_sync_config(m_dev, BLADERF_TX_X2, BLADERF_FORMAT_SC16_Q11, 128, 16384, 32, 1500);
        } else {
            status = bladerf_sync_config(m_dev, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11, 64, 8192, 32, 1500);
        }

        if (status < 0)
        {
            qCritical("BladeRF2OutputThread::run: cannot configure streams: %s", bladerf_strerror(status));
        }
        else
        {
            qDebug("BladeRF2OutputThread::run: start running loop");

            while (m_running)
            {
                if (m_nbChannels > 1)
                {
                    callbackMO(m_buf, DeviceBladeRF2::blockSize);
                    res = bladerf_sync_tx(m_dev, m_buf, DeviceBladeRF2::blockSize*m_nbChannels, 0, 1500);
                }
                else
                {
                    callbackSO(m_buf, DeviceBladeRF2::blockSize);
                    res = bladerf_sync_tx(m_dev, m_buf, DeviceBladeRF2::blockSize, 0, 1500);
                }

                if (res < 0)
                {
                    qCritical("BladeRF2OutputThread::run sync Rx error: %s", bladerf_strerror(res));
                    break;
                }
            }

            qDebug("BladeRF2OutputThread::run: stop running loop");
        }
    }
    else
    {
        qWarning("BladeRF2OutputThread::run: no channels or FIFO allocated. Aborting");
    }


    m_running = false;
}

unsigned int BladeRF2OutputThread::getNbFifos()
{
    unsigned int fifoCount = 0;

    for (unsigned int i = 0; i < m_nbChannels; i++)
    {
        if (m_channels[i].m_sampleFifo) {
            fifoCount++;
        }
    }

    return fifoCount;
}

void BladeRF2OutputThread::setLog2Interpolation(unsigned int channel, unsigned int log2_interp)
{
    if (channel < m_nbChannels) {
        m_channels[channel].m_log2Interp = log2_interp;
    }
}

unsigned int BladeRF2OutputThread::getLog2Interpolation(unsigned int channel) const
{
    if (channel < m_nbChannels) {
        return m_channels[channel].m_log2Interp;
    } else {
        return 0;
    }
}

void BladeRF2OutputThread::setFifo(unsigned int channel, SampleSourceFifo *sampleFifo)
{
    if (channel < m_nbChannels) {
        m_channels[channel].m_sampleFifo = sampleFifo;
    }
}

SampleSourceFifo *BladeRF2OutputThread::getFifo(unsigned int channel)
{
    if (channel < m_nbChannels) {
        return m_channels[channel].m_sampleFifo;
    } else {
        return 0;
    }
}

void BladeRF2OutputThread::callbackMO(qint16* buf, qint32 samplesPerChannel)
{
    for (unsigned int channel = 0; channel < m_nbChannels; channel++)
    {
        if (m_channels[channel].m_sampleFifo) {
            callbackSO(&buf[2*samplesPerChannel*channel], samplesPerChannel, channel);
        } else {
            std::fill(&buf[2*samplesPerChannel*channel], &buf[2*samplesPerChannel*channel]+2*samplesPerChannel, 0); // fill with zero samples
        }
    }

    // TODO: write a set of interpolators that can write interleaved samples in output directly
    int status = bladerf_interleave_stream_buffer(BLADERF_TX_X2, BLADERF_FORMAT_SC16_Q11 , samplesPerChannel*m_nbChannels, (void *) buf);

    if (status < 0)
    {
        qCritical("BladeRF2OutputThread::callbackMI: cannot interleave buffer: %s", bladerf_strerror(status));
        return;
    }
}

//  Interpolate according to specified log2 (ex: log2=4 => decim=16). len is a number of samples (not a number of I or Q)
void BladeRF2OutputThread::callbackSO(qint16* buf, qint32 len, unsigned int channel)
{
    if (m_channels[channel].m_sampleFifo)
    {
        float bal = m_channels[channel].m_sampleFifo->getRWBalance();

        if (bal < -0.25) {
            qDebug("BladeRF2OutputThread::callbackSO: read lags: %f", bal);
        } else if (bal > 0.25) {
            qDebug("BladeRF2OutputThread::callbackSO: read leads: %f", bal);
        }

        SampleVector::iterator beginRead;
        m_channels[channel].m_sampleFifo->readAdvance(beginRead, len/(1<<m_channels[channel].m_log2Interp));
        beginRead -= len;

        if (m_channels[channel].m_log2Interp == 0)
        {
            m_channels[channel].m_interpolators.interpolate1(&beginRead, buf, len*2);
        }
        else
        {
            switch (m_channels[channel].m_log2Interp)
            {
            case 1:
                m_channels[channel].m_interpolators.interpolate2_cen(&beginRead, buf, len*2);
                break;
            case 2:
                m_channels[channel].m_interpolators.interpolate4_cen(&beginRead, buf, len*2);
                break;
            case 3:
                m_channels[channel].m_interpolators.interpolate8_cen(&beginRead, buf, len*2);
                break;
            case 4:
                m_channels[channel].m_interpolators.interpolate16_cen(&beginRead, buf, len*2);
                break;
            case 5:
                m_channels[channel].m_interpolators.interpolate32_cen(&beginRead, buf, len*2);
                break;
            case 6:
                m_channels[channel].m_interpolators.interpolate64_cen(&beginRead, buf, len*2);
                break;
            default:
                break;
            }
        }
    }
    else
    {
        std::fill(buf, buf+2*len, 0);
    }
}

