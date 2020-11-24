//***************************************************************************
// Copyright 2007-2020 Universidade do Porto - Faculdade de Engenharia      *
// Laboratório de Sistemas e Tecnologia Subaquática (LSTS)                  *
//***************************************************************************
// This file is part of DUNE: Unified Navigation Environment.               *
//                                                                          *
// Commercial Licence Usage                                                 *
// Licencees holding valid commercial DUNE licences may use this file in    *
// accordance with the commercial licence agreement provided with the       *
// Software or, alternatively, in accordance with the terms contained in a  *
// written agreement between you and Faculdade de Engenharia da             *
// Universidade do Porto. For licensing terms, conditions, and further      *
// information contact lsts@fe.up.pt.                                       *
//                                                                          *
// Modified European Union Public Licence - EUPL v.1.1 Usage                *
// Alternatively, this file may be used under the terms of the Modified     *
// EUPL, Version 1.1 only (the "Licence"), appearing in the file LICENCE.md *
// included in the packaging of this file. You may not use this work        *
// except in compliance with the Licence. Unless required by applicable     *
// law or agreed to in writing, software distributed under the Licence is   *
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF     *
// ANY KIND, either express or implied. See the Licence for the specific    *
// language governing permissions and limitations at                        *
// https://github.com/LSTS/dune/blob/master/LICENCE.md and                  *
// http://ec.europa.eu/idabc/eupl.html.                                     *
//***************************************************************************
// Author: Behdad Aminian                                                   *
//***************************************************************************

// ISO C++ 98 headers
#include <vector>
#include <stdexcept>
#include <set>
#include <cmath>

// DUNE headers.
#include <DUNE/DUNE.hpp>

namespace Transports
{
  namespace Consensus
  {
    using DUNE_NAMESPACES;

    struct Destination
    {
      // Destination address.
      Address addr;
      // Destination port.
      unsigned port;
      // True if address is local.
      bool local;
    };

    struct Arguments
    {
      // Ports.
      std::vector<unsigned> ports;
      // True if multicast is enabled.
      bool enable_mcast;
      // True of broadcast is enabled.
      bool enable_bcast;
      // True of loopback is enabled.
      bool enable_lback;
      // Multicast address.
      Address addr_mcast;
      // // Ignored interfaces.
      std::vector<std::string> ignored_interfaces;
      //Maximum acceptable salinity differences
      float max_acceptable_salinity_difrences;
    };

    struct Task : public DUNE::Tasks::Task
    {
      // Local advertising buffer.
      uint8_t m_bfr_loc[4096];
      // External advertising buffer.
      uint8_t m_bfr_ext[4096];
      // Socket.
      UDPSocket m_sock;
      // List of destinations.
      std::vector<Destination> m_dsts;
      // Task arguments.
      Arguments m_args;

      // Last received salinity.
      IMC::Salinity *m_salinity_received;

      // Last calculated salinity local
      IMC::Salinity m_salinity_calculated_local;

      // Last calculated salinity local
      IMC::Salinity m_salinity_calculated_ext;
      // True if all node agreed on level of salinity.
      bool is_consensus_reached;

      Task(const std::string &name, Tasks::Context &ctx) : DUNE::Tasks::Task(name, ctx)
      {
        // Define configuration parameters.
        param("Delta", m_args.max_acceptable_salinity_difrences)
            .defaultValue("10")
            .description("Max salinity differences");

        param("Enable Loopback", m_args.enable_lback)
            .defaultValue("false")
            .description("Enable announcing on loopback interfaces");

        param("Enable Multicast", m_args.enable_mcast)
            .defaultValue("true")
            .description("Enable multicast announcing");

        param("Enable Broadcast", m_args.enable_bcast)
            .defaultValue("true")
            .description("Enable broadcast announcing");

        param("Ports", m_args.ports)
            .defaultValue("30100, 30101, 30102, 30103, 30104")
            .description("List of destination ports");

        param("Multicast Address", m_args.addr_mcast)
            .defaultValue("224.0.75.69")
            .description("Multicast address");

        param("Ignored Interfaces", m_args.ignored_interfaces)
            .defaultValue("eth0:prv")
            .description("List of interfaces whose services will not be announced");

        // Register listeners.
        bind<IMC::Salinity>(this);
      }

      ~Task(void)
      {
        if (m_salinity_received)
          delete m_salinity_received;
      }

      void
      onResourceInitialization(void)
      {
        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_ACTIVE);
      }

      void
      onUpdateParameters(void)
      {
      }

      void
      consume(const IMC::Salinity *msg)
      {
        if (m_salinity_received)
          *m_salinity_received = *msg;
        else
          m_salinity_received = new IMC::Salinity(*msg);

        calculateAndAnnounce();
      }

      void
      probeInterfaces(void)
      {
        m_dsts.clear();

        // Setup loopback.
        if (m_args.enable_lback)
        {
          for (unsigned i = 0; i < m_args.ports.size(); ++i)
          {
            Destination dst;
            dst.port = m_args.ports[i];
            dst.addr = "127.0.0.1";
            dst.local = true;
            m_dsts.push_back(dst);
          }
        }

        // Setup multicast.
        if (m_args.enable_mcast)
        {
          m_sock.setMulticastLoop(false);
          for (unsigned i = 0; i < m_args.ports.size(); ++i)
          {
            Destination dst;
            dst.port = m_args.ports[i];
            dst.addr = m_args.addr_mcast;
            dst.local = false;
            m_dsts.push_back(dst);
          }
        }

        // Setup broadcast.
        if (m_args.enable_bcast)
        {
          m_sock.enableBroadcast(true);

          for (unsigned j = 0; j < m_args.ports.size(); ++j)
          {
            Destination dst;
            dst.port = m_args.ports[j];
            dst.addr = "255.255.255.255";
            dst.local = false;
            m_dsts.push_back(dst);
          }

          std::vector<Interface> itfs = Interface::get();
          for (unsigned i = 0; i < itfs.size(); ++i)
          {
            // Discard loopback addresses.
            if (itfs[i].address().isLoopback() || itfs[i].broadcast().isAny())
              continue;

            for (unsigned j = 0; j < m_args.ports.size(); ++j)
            {
              Destination dst;
              dst.port = m_args.ports[j];
              dst.addr = itfs[i].broadcast();
              dst.local = false;
              m_dsts.push_back(dst);
            }
          }
        }
      }

      void
      calculateAndAnnounce(void)
      {

        if (abs(m_salinity_calculated_local.getValueFP()) < m_args.max_acceptable_salinity_difrences)
        {
          m_salinity_calculated_local.setValueFP(m_salinity_received->getValueFP() + 1);
          m_salinity_calculated_ext.setValueFP(m_salinity_calculated_local.getValueFP());
        }

        // We do this everytime because the number and configuration of
        // network interfaces might have changed.
        probeInterfaces();

        // Refresh serialized Salinity message.
        // m_salinity_calculated_local.
        m_salinity_calculated_local.setTimeStamp();
        uint16_t bfr_len_loc = IMC::Packet::serialize(&m_salinity_calculated_local, m_bfr_loc, sizeof(m_bfr_loc));
        m_salinity_calculated_ext.setTimeStamp();
        uint16_t bfr_len_ext = IMC::Packet::serialize(&m_salinity_calculated_ext, m_bfr_ext, sizeof(m_bfr_ext));

        dispatch(m_salinity_calculated_local);
        dispatch(m_salinity_calculated_ext);

        for (unsigned i = 0; i < m_dsts.size(); ++i)
        {
          try
          {
            if (m_dsts[i].local)
              m_sock.write(m_bfr_loc, bfr_len_loc, m_dsts[i].addr, m_dsts[i].port);
            else
              m_sock.write(m_bfr_ext, bfr_len_ext, m_dsts[i].addr, m_dsts[i].port);
          }
          catch (...)
          {
          }
        }
      }

      void
      onMain(void)
      {
        while (!stopping())
        {

          if (!m_salinity_calculated_local.getValueFP())
          {
            calculateAndAnnounce();
          }
          consumeMessages();
        }
      }
    };
  } // namespace Consensus
} // namespace Transports

DUNE_TASK
