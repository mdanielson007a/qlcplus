/*
  Q Light Controller Plus
  universe.cpp

  Copyright (c) Massimo Callegari

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <QDebug>
#include <QDomElement>
#include <QDomNode>
#include <math.h>

#include "channelmodifier.h"
#include "inputoutputmap.h"
#include "qlcioplugin.h"
#include "outputpatch.h"
#include "grandmaster.h"
#include "inputpatch.h"
#include "qlcmacros.h"
#include "universe.h"
#include "qlcfile.h"
#include "utils.h"

#define RELATIVE_ZERO 127

#define KXMLUniverseNormalBlend "Normal"
#define KXMLUniverseMaskBlend "Mask"
#define KXMLUniverseAdditiveBlend "Additive"
#define KXMLUniverseSubtractiveBlend "Subtractive"

Universe::Universe(quint32 id, GrandMaster *gm, QObject *parent)
    : QObject(parent)
    , m_id(id)
    , m_grandMaster(gm)
    , m_passthrough(false)
    , m_monitor(false)
    , m_inputPatch(NULL)
    , m_outputPatch(NULL)
    , m_fbPatch(NULL)
    , m_channelsMask(new QByteArray(UNIVERSE_SIZE, char(0)))
    , m_modifiedZeroValues(new QByteArray(UNIVERSE_SIZE, char(0)))
    , m_usedChannels(0)
    , m_totalChannels(0)
    , m_totalChannelsChanged(false)
    , m_intensityChannelsChanged(false)
    , m_preGMValues(new QByteArray(UNIVERSE_SIZE, char(0)))
    , m_postGMValues(new QByteArray(UNIVERSE_SIZE, char(0)))
    , m_lastPostGMValues(new QByteArray(UNIVERSE_SIZE, char(0)))
{
    m_relativeValues.fill(0, UNIVERSE_SIZE);
    m_modifiers.fill(NULL, UNIVERSE_SIZE);

    m_name = QString("Universe %1").arg(id + 1);

    connect(m_grandMaster, SIGNAL(valueChanged(uchar)),
            this, SLOT(slotGMValueChanged()));
}

Universe::~Universe()
{
    delete m_inputPatch;
    delete m_outputPatch;
    delete m_fbPatch;
}

void Universe::setName(QString name)
{
    if (name.isEmpty())
        m_name = QString("Universe %1").arg(m_id + 1);
    else
        m_name = name;
    emit nameChanged();
}

QString Universe::name() const
{
    return m_name;
}

void Universe::setID(quint32 id)
{
    m_id = id;
}

quint32 Universe::id() const
{
    return m_id;
}

ushort Universe::usedChannels()
{
    return m_usedChannels;
}

ushort Universe::totalChannels()
{
    return m_totalChannels;
}

bool Universe::hasChanged()
{
    bool changed =
        memcmp(m_lastPostGMValues->constData(), m_postGMValues->constData(), m_usedChannels) != 0;
    if (changed)
        memcpy(m_lastPostGMValues->data(), m_postGMValues->constData(), m_usedChannels);
    return changed;
}

void Universe::setPassthrough(bool enable)
{
    if (enable == m_passthrough)
        return;

    qDebug() << "Set universe" << id() << "passthrough to" << enable;

    if (m_inputPatch != NULL)
    {
        if (m_passthrough == false)
            disconnect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                       this, SIGNAL(inputValueChanged(quint32,quint32,uchar,QString)));
        else
            disconnect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                       this, SLOT(slotInputValueChanged(quint32,quint32,uchar,const QString&)));
    }

    m_passthrough = enable;

    if (m_inputPatch != NULL)
    {
        if (m_passthrough == false)
            connect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                    this, SIGNAL(inputValueChanged(quint32,quint32,uchar,QString)));
        else
            connect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                    this, SLOT(slotInputValueChanged(quint32,quint32,uchar,const QString&)));
    }
}

bool Universe::passthrough() const
{
    return m_passthrough;
}

void Universe::setMonitor(bool enable)
{
    m_monitor = enable;
}

bool Universe::monitor() const
{
    return m_monitor;
}

void Universe::slotGMValueChanged()
{
    {
        for (int i = 0; i < m_intensityChannels.size(); ++i)
        {
            int channel = m_intensityChannels.at(i);
            char chValue(m_preGMValues->at(channel));
            write(channel, chValue);
        }
    }

    if (m_grandMaster->channelMode() == GrandMaster::AllChannels)
    {
        for (int i = 0; i < m_nonIntensityChannels.size(); ++i)
        {
            int channel = m_nonIntensityChannels.at(i);
            char chValue(m_preGMValues->at(channel));
            write(channel, chValue);
        }
    }
}

/************************************************************************
 * Values
 ************************************************************************/

void Universe::reset()
{
    m_preGMValues->fill(0);
    m_postGMValues->fill(0);
    zeroRelativeValues();
    m_modifiers.fill(NULL, UNIVERSE_SIZE);
    m_passthrough = false;
}

void Universe::reset(int address, int range)
{
    if (address >= UNIVERSE_SIZE)
        return;
    if (address + range > UNIVERSE_SIZE) 
       range = UNIVERSE_SIZE - address;

    memset(m_preGMValues->data() + address, 0, range * sizeof(*m_preGMValues->data()));
    memset(m_relativeValues.data() + address, 0, range * sizeof(*m_relativeValues.data()));
    memcpy(m_postGMValues->data() + address, m_modifiedZeroValues->data() + address, range * sizeof(*m_postGMValues->data()));
}

void Universe::zeroIntensityChannels()
{
    updateIntensityChannelsRanges();
    int const* channels = m_intensityChannelsRanges.constData();
    for (int i = 0; i < m_intensityChannelsRanges.size(); ++i)
    {
        short channel = channels[i] >> 16;
        short size = channels[i] & 0xffff;

        memset(m_preGMValues->data() + channel, 0, size * sizeof(*m_preGMValues->data()));
        memcpy(m_postGMValues->data() + channel, m_modifiedZeroValues->data() + channel, size * sizeof(*m_postGMValues->data()));
    }
}

QHash<int, uchar> Universe::intensityChannels()
{
    QHash <int, uchar> intensityList;
    for (int i = 0; i < m_intensityChannels.size(); ++i)
    {
        int channel = m_intensityChannels.at(i);
        intensityList[channel] = m_preGMValues->at(channel);
    }
    return intensityList;
}

uchar Universe::postGMValue(int address) const
{
    if (address >= m_postGMValues->size())
        return 0;

    return uchar(m_postGMValues->at(address));
}

const QByteArray* Universe::postGMValues() const
{
    return m_postGMValues.data();
}

void Universe::zeroRelativeValues()
{
    memset(m_relativeValues.data(), 0, UNIVERSE_SIZE * sizeof(*m_relativeValues.data()));
}

Universe::BlendMode Universe::stringToBlendMode(QString mode)
{
    if (mode == KXMLUniverseNormalBlend)
        return NormalBlend;
    else if (mode == KXMLUniverseMaskBlend)
        return MaskBlend;
    else if (mode == KXMLUniverseAdditiveBlend)
        return AdditiveBlend;
    else if (mode == KXMLUniverseSubtractiveBlend)
        return SubtractiveBlend;

    return NormalBlend;
}

QString Universe::blendModeToString(Universe::BlendMode mode)
{
    switch(mode)
    {
        default:
        case NormalBlend:
            return QString(KXMLUniverseNormalBlend);
        break;
        case MaskBlend:
            return QString(KXMLUniverseMaskBlend);
        break;
        case AdditiveBlend:
            return QString(KXMLUniverseAdditiveBlend);
        break;
        case SubtractiveBlend:
            return QString(KXMLUniverseSubtractiveBlend);
        break;
    }
}

const QByteArray Universe::preGMValues() const
{
    return *m_preGMValues;
}

uchar Universe::preGMValue(int address) const
{
    if (address >= m_preGMValues->size())
        return 0;
 
    return uchar(m_preGMValues->at(address));
}

uchar Universe::applyGM(int channel, uchar value)
{
    if (value == 0)
        return (uchar)(*m_modifiedZeroValues)[channel];

    if ((m_grandMaster->channelMode() == GrandMaster::Intensity && m_channelsMask->at(channel) & Intensity) ||
        (m_grandMaster->channelMode() == GrandMaster::AllChannels))
    {
        if (m_grandMaster->valueMode() == GrandMaster::Limit)
            value = MIN(value, m_grandMaster->value());
        else
            value = char(floor((double(value) * m_grandMaster->fraction()) + 0.5));
    }

    if (m_modifiers.at(channel) != NULL)
        value = m_modifiers.at(channel)->getValue(value);

    return value;
}

/************************************************************************
 * Patches
 ************************************************************************/

bool Universe::isPatched()
{
    if (m_inputPatch != NULL || m_outputPatch != NULL || m_fbPatch != NULL)
        return true;

    return false;
}

bool Universe::setInputPatch(QLCIOPlugin *plugin,
                             quint32 input, QLCInputProfile *profile)
{
    qDebug() << "[Universe] setInputPatch - ID:" << m_id << ", plugin:" << ((plugin == NULL)?"None":plugin->name())
             << ", input:" << input << ", profile:" << ((profile == NULL)?"None":profile->name());
    if (m_inputPatch == NULL)
    {
        if (plugin == NULL || input == QLCIOPlugin::invalidLine())
            return true;

        m_inputPatch = new InputPatch(m_id, this);
        if (passthrough() == false)
            connect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                    this, SIGNAL(inputValueChanged(quint32,quint32,uchar,QString)));
        else
            connect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                    this, SLOT(slotInputValueChanged(quint32,quint32,uchar,const QString&)));
    }
    else
    {
        if (input == QLCIOPlugin::invalidLine())
        {
            if (passthrough() == false)
                disconnect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                        this, SIGNAL(inputValueChanged(quint32,quint32,uchar,QString)));
            else
                disconnect(m_inputPatch, SIGNAL(inputValueChanged(quint32,quint32,uchar,const QString&)),
                           this, SLOT(slotInputValueChanged(quint32,quint32,uchar,const QString&)));
            delete m_inputPatch;
            m_inputPatch = NULL;
            emit inputPatchChanged();
            return true;
        }
    }

    if (m_inputPatch != NULL)
    {
        bool result = m_inputPatch->set(plugin, input, profile);
        emit inputPatchChanged();
        return result;
    }

    return true;
}

bool Universe::setOutputPatch(QLCIOPlugin *plugin, quint32 output)
{
    qDebug() << "[Universe] setOutputPatch - ID:" << m_id
             << ", plugin:" << ((plugin == NULL)?"None":plugin->name()) << ", output:" << output;
    if (m_outputPatch == NULL)
    {
        if (plugin == NULL || output == QLCIOPlugin::invalidLine())
            return false;

        m_outputPatch = new OutputPatch(m_id, this);
    }
    else
    {
        if (output == QLCIOPlugin::invalidLine())
        {
            delete m_outputPatch;
            m_outputPatch = NULL;
            emit outputPatchChanged();
            return true;
        }
    }
    if (m_outputPatch != NULL)
    {
        bool result = m_outputPatch->set(plugin, output);
        emit outputPatchChanged();
        return result;
    }

    return false;
}

bool Universe::setFeedbackPatch(QLCIOPlugin *plugin, quint32 output)
{
    qDebug() << Q_FUNC_INFO << "plugin:" << plugin << "output:" << output;
    if (m_fbPatch == NULL)
    {
        if (output == QLCIOPlugin::invalidLine())
            return false;
        m_fbPatch = new OutputPatch(m_id, this);
    }
    else
    {
        if (output == QLCIOPlugin::invalidLine())
        {
            delete m_fbPatch;
            m_fbPatch = NULL;
            return true;
        }
    }
    if (m_fbPatch != NULL)
        return m_fbPatch->set(plugin, output);

    return false;
}

InputPatch *Universe::inputPatch() const
{
    return m_inputPatch;
}

OutputPatch *Universe::outputPatch() const
{
    return m_outputPatch;
}

OutputPatch *Universe::feedbackPatch() const
{
    return m_fbPatch;
}

void Universe::dumpOutput(const QByteArray &data)
{
    if (m_outputPatch == NULL)
        return;

    if (m_totalChannelsChanged == true)
    {
        m_outputPatch->setPluginParameter(PLUGIN_UNIVERSECHANNELS, m_totalChannels);
        m_totalChannelsChanged = false;
    }
    m_outputPatch->dump(m_id, data);
}

void Universe::flushInput()
{
    if (m_inputPatch == NULL)
        return;

    m_inputPatch->flush(m_id);
}

void Universe::slotInputValueChanged(quint32 universe, quint32 channel, uchar value, const QString &key)
{
    if (m_passthrough == true)
    {
        if (universe == m_id)
            write(channel, value);
    }
    else
        emit inputValueChanged(universe, channel, value, key);
}

/************************************************************************
 * Channels capabilities
 ************************************************************************/

void Universe::setChannelCapability(ushort channel, QLCChannel::Group group, ChannelType forcedType)
{
    if (channel >= (ushort)m_channelsMask->count())
        return;

    if (Utils::vectorRemove(m_intensityChannels, channel))
        m_intensityChannelsChanged = true;
    Utils::vectorRemove(m_nonIntensityChannels, channel);

    if (forcedType != Undefined)
    {
        (*m_channelsMask)[channel] = char(forcedType);
        if (forcedType == HTP)
        {
            //qDebug() << "--- Channel" << channel << "forced type HTP";
            Utils::vectorSortedAddUnique(m_intensityChannels, channel);
            m_intensityChannelsChanged = true;
            if (group == QLCChannel::Intensity)
            {
                //qDebug() << "--- Channel" << channel << "Intensity + HTP";
                (*m_channelsMask)[channel] = char(HTP | Intensity);
            }
        }
        else if (forcedType == LTP)
        {
            //qDebug() << "--- Channel" << channel << "forced type LTP";
            Utils::vectorSortedAddUnique(m_nonIntensityChannels, channel);
        }
    }
    else
    {
        if (group == QLCChannel::Intensity)
        {
            //qDebug() << "--- Channel" << channel << "Intensity + HTP";
            (*m_channelsMask)[channel] = char(HTP | Intensity);
            Utils::vectorSortedAddUnique(m_intensityChannels, channel);
            m_intensityChannelsChanged = true;
        }
        else
        {
            //qDebug() << "--- Channel" << channel << "LTP";
            (*m_channelsMask)[channel] = char(LTP);
            Utils::vectorSortedAddUnique(m_nonIntensityChannels, channel);
        }
    }

    // qDebug() << Q_FUNC_INFO << "Channel:" << channel << "mask:" << QString::number(m_channelsMask->at(channel), 16);
    if (channel >= m_totalChannels)
    {
        m_totalChannels = channel + 1;
        m_totalChannelsChanged = true;
    }

    return;
}

uchar Universe::channelCapabilities(ushort channel)
{
    if (channel >= (ushort)m_channelsMask->count())
        return Undefined;

    return m_channelsMask->at(channel);
}

void Universe::setChannelModifier(ushort channel, ChannelModifier *modifier)
{
    if (channel >= (ushort)m_modifiers.count())
        return;

    m_modifiers[channel] = modifier;

    (*m_modifiedZeroValues)[channel] =
        (modifier == NULL ? uchar(0) : modifier->getValue(0));
}

ChannelModifier *Universe::channelModifier(ushort channel)
{
    if (channel >= (ushort)m_modifiers.count())
        return NULL;

    return m_modifiers.at(channel);
}

void Universe::updateIntensityChannelsRanges()
{
    if (!m_intensityChannelsChanged)
        return;
    m_intensityChannelsChanged = false;

    m_intensityChannelsRanges.clear();
    short currentPos = -1;
    short currentSize = 0;

    for (int i = 0; i < m_intensityChannels.size(); ++i)
    {
        int channel = m_intensityChannels.at(i);
        if (currentPos + currentSize == channel)
            ++currentSize;
        else
        {
            if (currentPos != -1)
                m_intensityChannelsRanges.append((currentPos << 16) | currentSize);
            currentPos = channel;
            currentSize = 1;
        }
    }
    if (currentPos != -1)
        m_intensityChannelsRanges.append((currentPos << 16) | currentSize);

    qDebug() << Q_FUNC_INFO << ":" << m_intensityChannelsRanges.size() << "ranges";
}

/****************************************************************************
 * Writing
 ****************************************************************************/

bool Universe::write(int channel, uchar value, bool forceLTP)
{
    Q_ASSERT(channel < UNIVERSE_SIZE);

    //qDebug() << "Universe write channel" << channel << ", value:" << value;

    if (channel >= m_usedChannels)
        m_usedChannels = channel + 1;

    if (forceLTP == false && (m_channelsMask->at(channel) & HTP) && value < (uchar)m_preGMValues->at(channel))
    {
        qDebug() << "[Universe] HTP check not passed" << channel << value;
        return false;
    }

    (*m_preGMValues)[channel] = char(value);

    if (m_relativeValues[channel] != 0)
    {
        int val = m_relativeValues[channel] + value; // value == (uchar)m_preGMValues->at(channel)
        value = CLAMP(val, 0, (int)UCHAR_MAX);
    }

    (*m_postGMValues)[channel] = applyGM(channel, value);

    return true;
}

bool Universe::writeRelative(int channel, uchar value)
{
    Q_ASSERT(channel < UNIVERSE_SIZE);

    if (channel >= m_usedChannels)
        m_usedChannels = channel + 1;

    if (value == RELATIVE_ZERO)
        return true;

    m_relativeValues[channel] += value - RELATIVE_ZERO;

    int val = m_relativeValues[channel];
    val += (uchar)m_preGMValues->at(channel);
    value = CLAMP(val, 0, (int)UCHAR_MAX);

    (*m_postGMValues)[channel] = applyGM(channel, value);

    return true;
}

bool Universe::writeBlended(int channel, uchar value, Universe::BlendMode blend)
{
    if (channel >= m_usedChannels)
        m_usedChannels = channel + 1;

    switch (blend)
    {
        case NormalBlend:
            return write(channel, value);
        break;
        case MaskBlend:
        {
            if (value)
            {
                float currValue = (float)uchar(m_preGMValues->at(channel));
                if (currValue)
                    value = currValue * ((float)value / 255.0);
                else
                    value = 0;
            }
            (*m_preGMValues)[channel] = char(value);

        }
        break;
        case AdditiveBlend:
        {
            uchar currVal = uchar(m_preGMValues->at(channel));
            value = qMin((int)currVal + value, 255);
            (*m_preGMValues)[channel] = char(value);
        }
        break;
        case SubtractiveBlend:
        {
            uchar currVal = uchar(m_preGMValues->at(channel));
            if (value >= currVal)
                value = 0;
            else
                value = currVal - value;
            (*m_preGMValues)[channel] = char(value);
        }
        break;
        default:
            qDebug() << "[Universe] Blend mode not handled. Implement me !" << blend;
        break;
    }

    (*m_postGMValues)[channel] = applyGM(channel, value);

    return true;
}

bool Universe::loadXML(const QDomElement &root, int index, InputOutputMap *ioMap)
{
    if (root.tagName() != KXMLQLCUniverse)
    {
        qWarning() << Q_FUNC_INFO << "Universe node not found";
        return false;
    }

    if (root.hasAttribute(KXMLQLCUniverseName))
        setName(root.attribute(KXMLQLCUniverseName));

    if (root.hasAttribute(KXMLQLCUniversePassthrough))
    {
        if (root.attribute(KXMLQLCUniversePassthrough) == KXMLQLCTrue ||
            root.attribute(KXMLQLCUniversePassthrough) == "1")
            setPassthrough(true);
        else
            setPassthrough(false);
    }

    QDomNode node = root.firstChild();
    while (node.isNull() == false)
    {
        QDomElement tag = node.toElement();

        if (tag.tagName() == KXMLQLCUniverseInputPatch)
        {
            QString plugin = KInputNone;
            quint32 input = QLCIOPlugin::invalidLine();
            QString profile = KInputNone;

            if (tag.hasAttribute(KXMLQLCUniverseInputPlugin))
                plugin = tag.attribute(KXMLQLCUniverseInputPlugin);
            if (tag.hasAttribute(KXMLQLCUniverseInputLine))
                input = tag.attribute(KXMLQLCUniverseInputLine).toUInt();
            if (tag.hasAttribute(KXMLQLCUniverseInputProfileName))
                profile = tag.attribute(KXMLQLCUniverseInputProfileName);
            ioMap->setInputPatch(index, plugin, input, profile);

            // load the plugin custom parameters, if present
            if (tag.hasChildNodes())
            {
                InputPatch *ip = inputPatch();
                if (ip != NULL)
                {
                    QDomNode patchNode = tag.firstChild();
                    while (patchNode.isNull() == false)
                    {
                        QDomElement pluginTag = patchNode.toElement();
                        if (pluginTag.tagName() == KXMLQLCUniversePluginParameters)
                        {
                             QDomNamedNodeMap attrs = pluginTag.attributes();
                             for (int i = 0; i < attrs.count(); i++)
                             {
                                 QDomAttr attr = attrs.item(i).toAttr();
                                 ip->setPluginParameter(attr.name(), attr.value());
                             }
                        }
                        patchNode = patchNode.nextSibling();
                    }
                }
            }
        }
        else if (tag.tagName() == KXMLQLCUniverseOutputPatch)
        {
            QString plugin = KOutputNone;
            quint32 output = QLCIOPlugin::invalidLine();
            if (tag.hasAttribute(KXMLQLCUniverseOutputPlugin))
                plugin = tag.attribute(KXMLQLCUniverseOutputPlugin);
            if (tag.hasAttribute(KXMLQLCUniverseOutputLine))
                output = tag.attribute(KXMLQLCUniverseOutputLine).toUInt();
            ioMap->setOutputPatch(index, plugin, output, false);

            // load the plugin custom parameters, if present
            if (tag.hasChildNodes())
            {
                OutputPatch *op = outputPatch();
                if (op != NULL)
                {
                    QDomNode patchNode = tag.firstChild();
                    while (patchNode.isNull() == false)
                    {
                        QDomElement pluginTag = patchNode.toElement();
                        if (pluginTag.tagName() == KXMLQLCUniversePluginParameters)
                        {
                             QDomNamedNodeMap attrs = pluginTag.attributes();
                             for (int i = 0; i < attrs.count(); i++)
                             {
                                 QDomAttr attr = attrs.item(i).toAttr();
                                 op->setPluginParameter(attr.name(), attr.value());
                             }
                        }
                        patchNode = patchNode.nextSibling();
                    }
                }
            }
        }
        else if (tag.tagName() == KXMLQLCUniverseFeedbackPatch)
        {
            QString plugin = KOutputNone;
            quint32 output = QLCIOPlugin::invalidLine();
            if (tag.hasAttribute(KXMLQLCUniverseFeedbackPlugin))
                plugin = tag.attribute(KXMLQLCUniverseFeedbackPlugin);
            if (tag.hasAttribute(KXMLQLCUniverseFeedbackLine))
                output = tag.attribute(KXMLQLCUniverseFeedbackLine).toUInt();
            ioMap->setOutputPatch(index, plugin, output, true);

            // load the plugin custom parameters, if present
            if (tag.hasChildNodes())
            {
                OutputPatch *fbp = feedbackPatch();
                if (fbp != NULL)
                {
                    QDomNode patchNode = tag.firstChild();
                    while (patchNode.isNull() == false)
                    {
                        QDomElement pluginTag = patchNode.toElement();
                        if (pluginTag.tagName() == KXMLQLCUniversePluginParameters)
                        {
                             QDomNamedNodeMap attrs = pluginTag.attributes();
                             for (int i = 0; i < attrs.count(); i++)
                             {
                                 QDomAttr attr = attrs.item(i).toAttr();
                                 fbp->setPluginParameter(attr.name(), attr.value());
                             }
                        }
                        patchNode = patchNode.nextSibling();
                    }
                }
            }
        }

        node = node.nextSibling();
    }

    return true;
}

/*********************************************************************
 * Load & Save
 *********************************************************************/

bool Universe::saveXML(QDomDocument *doc, QDomElement *wksp_root) const
{
    Q_ASSERT(doc != NULL);
    Q_ASSERT(wksp_root != NULL);

    QDomElement root = doc->createElement(KXMLQLCUniverse);
    root.setAttribute(KXMLQLCUniverseName, name());
    root.setAttribute(KXMLQLCUniverseID, id());
    if (passthrough() == true)
        root.setAttribute(KXMLQLCUniversePassthrough, KXMLQLCTrue);
    else
        root.setAttribute(KXMLQLCUniversePassthrough, KXMLQLCFalse);

    if (inputPatch() != NULL)
    {
        QDomElement ip = doc->createElement(KXMLQLCUniverseInputPatch);
        ip.setAttribute(KXMLQLCUniverseInputPlugin, inputPatch()->pluginName());
        ip.setAttribute(KXMLQLCUniverseInputLine, inputPatch()->input());
        ip.setAttribute(KXMLQLCUniverseInputProfileName, inputPatch()->profileName());
        savePluginParametersXML(doc, &ip, inputPatch()->getPluginParameters());
        root.appendChild(ip);
    }
    if (outputPatch() != NULL)
    {
        QDomElement op = doc->createElement(KXMLQLCUniverseOutputPatch);
        op.setAttribute(KXMLQLCUniverseOutputPlugin, outputPatch()->pluginName());
        op.setAttribute(KXMLQLCUniverseOutputLine, outputPatch()->output());
        savePluginParametersXML(doc, &op, outputPatch()->getPluginParameters());
        root.appendChild(op);
    }
    if (feedbackPatch() != NULL)
    {
        QDomElement fbp = doc->createElement(KXMLQLCUniverseFeedbackPatch);
        fbp.setAttribute(KXMLQLCUniverseFeedbackPlugin, feedbackPatch()->pluginName());
        fbp.setAttribute(KXMLQLCUniverseFeedbackLine, feedbackPatch()->output());
        savePluginParametersXML(doc, &fbp, feedbackPatch()->getPluginParameters());
        root.appendChild(fbp);
    }

    // append universe element
    wksp_root->appendChild(root);

    return true;
}

bool Universe::savePluginParametersXML(QDomDocument *doc, QDomElement *wksp_root,
                                       QMap<QString, QVariant> parameters) const
{
    Q_ASSERT(doc != NULL);
    Q_ASSERT(wksp_root != NULL);

    if (parameters.isEmpty())
        return false;

    QDomElement pp = doc->createElement(KXMLQLCUniversePluginParameters);
    QMapIterator<QString, QVariant> it(parameters);
    while(it.hasNext())
    {
        it.next();
        QString pName = it.key();
        QVariant pValue = it.value();
        pp.setAttribute(pName, pValue.toString());
    }
    wksp_root->appendChild(pp);

    return true;
}

