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
      float max_acceptable_salinity;
      // Trace incoming messages.
      bool trace_in;
      // Represting the measured salinity.
      uint8_t measured_salinity;
    };

    struct Task : public DUNE::Tasks::Task
    {
      // Local advertising buffer.
      uint8_t m_bfr_loc[4096];
      // Socket.
      UDPSocket m_sock;
      // List of destinations.
      std::vector<Destination> m_dsts;
      // Task arguments.
      Arguments m_args;

      // Last received salinity.
      IMC::Salinity *m_salinity_received;

      // Last calculated salinity local
      IMC::Salinity m_salinity_estimated_local;

      // Our DUNE's UID URL.
      std::string m_dune_uid;
      // Last timestamps.
      std::map<Address, double> m_tstamps;
      // Deserialization buffer.
      uint8_t m_bfr[4096];

      bool is_message_received;

      Task(const std::string &name, Tasks::Context &ctx) : DUNE::Tasks::Task(name, ctx)
      {
        // Define configuration parameters.
        param("Delta", m_args.max_acceptable_salinity)
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
            .defaultValue("31100, 31101, 31102, 31103, 31104")
            .description("List of destination ports");

        param("Multicast Address", m_args.addr_mcast)
            .defaultValue("224.0.75.69")
            .description("Multicast address");

        param("Ignored Interfaces", m_args.ignored_interfaces)
            .defaultValue("eth0:prv")
            .description("List of interfaces whose services will not be announced");

        param("Print Incoming Messages", m_args.trace_in)
            .defaultValue("false")
            .description("Print incoming messages (Debug)");

        param("Measured salinity", m_args.measured_salinity)
            .defaultValue("1")
            .description("Representing the measured salinity.");

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

        // Initialize socket.
        m_sock.setMulticastTTL(1);
        m_sock.setMulticastLoop(false);
        m_sock.enableBroadcast(true);

        std::vector<Interface> itfs = Interface::get();
        for (unsigned i = 0; i < itfs.size(); ++i)
          m_sock.joinMulticastGroup(m_args.addr_mcast, itfs[i].address());

        for (unsigned i = 0; i < m_args.ports.size(); ++i)
        {
          try
          {
            m_sock.bind(m_args.ports[i], Address::Any, false);
            inf(DTR("listening on %s:%u"), Address(Address::Any).c_str(), m_args.ports[i]);
            setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_ACTIVE);
            return;
          }
          catch (...)
          {
          }
        }

        throw std::runtime_error(DTR("no available ports to listen to advertisements"));
      }

      void
      readMessage(UDPSocket &sock)
      {
        inf(DTR("Reading messages started."));

        Address addr;
        uint16_t rv = sock.read(m_bfr, sizeof(m_bfr), &addr);
        IMC::Message *msg = IMC::Packet::deserialize(m_bfr, rv);

        // Validate message.
        if (msg == 0)
        {
          war(DTR("discarding spurious message"));
          delete msg;
          return;
        }

        if (msg->getId() != DUNE_IMC_SALINITY)
        {
          war(DTR("discarding spurious message '%s'"), msg->getName());
          delete msg;
          return;
        }

        // Check if we already got this message.
        std::map<Address, double>::iterator itr = m_tstamps.find(addr);
        if (itr != m_tstamps.end())
        {
          if (itr->second == msg->getTimeStamp())
          {

            delete msg;
            return;
          }
          else
          {

            itr->second = msg->getTimeStamp();
          }
        }
        else
        {

          m_tstamps[addr] = msg->getTimeStamp();
        }

        // Discarding messages that come from same dune resource
        if (m_salinity_estimated_local.getSource() == msg->getSource())
        {
          war(DTR("Discarding the message from the same dune"));
          delete msg;
          return;
        }

        //Remaining message can be casted to salinity.
        m_salinity_received = static_cast<IMC::Salinity *>(msg);

        // Check if the message was sent from our computer.
        bool m_local = false;
        std::vector<Interface> itfs = Interface::get();
        for (unsigned i = 0; i < itfs.size(); ++i)
        {
          if (itfs[i].address() == addr)
          {
            m_local = true;
            break;
          }
        }

        // Send to other tasks.
        dispatch(msg, DF_KEEP_TIME);

        if (m_args.trace_in)
          msg->toText(std::cerr);

        delete msg;

        inf(DTR("Reading message completed."));

        //Call the stimation method to restimate the salinity using the last received message.
        stimateSalinity();
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
      stimateSalinity(void)
      {

        inf(DTR(" Salinity stimation started "));

        //In the first iteration and haven't recieved any salinity yet, the measured salinity will be used
        if (!m_salinity_received)
        {
          m_salinity_estimated_local.setValueFP(m_args.measured_salinity);
        }
        else if (abs(m_salinity_estimated_local.getValueFP()) < m_args.max_acceptable_salinity)
        {

          m_salinity_estimated_local.setValueFP(m_salinity_received->value + m_salinity_estimated_local.getValueFP());

          m_salinity_received->value = 0;
        }
        else
        {
          m_salinity_estimated_local.setValueFP(m_args.max_acceptable_salinity);
        }

        inf(DTR("Salinity stimation is done."));

        //Sending the stimated salinity to other vehicles.
        announceEstimatedSalinity();
      }

      void
      announceEstimatedSalinity(void)
      {

        inf(DTR("Announcing the estimated salinity started."));

        probeInterfaces();

        // m_salinity_estimated_local.
        m_salinity_estimated_local.setTimeStamp();
        uint16_t bfr_len_loc = IMC::Packet::serialize(&m_salinity_estimated_local, m_bfr_loc, sizeof(m_bfr_loc));

        dispatch(m_salinity_estimated_local);

        for (unsigned i = 0; i < m_dsts.size(); ++i)
        {
          try
          {
            if (!m_dsts[i].local)
              m_sock.write(m_bfr_loc, bfr_len_loc, m_dsts[i].addr, m_dsts[i].port);
            inf(DTR("Wrting new estimated value  %F on UDP"), m_salinity_estimated_local.getValueFP());
          }
          catch (...)
          {
          }
        }

        inf(DTR("Announcing Estimated salinity is done!"));
      }

     
      void
      onMain(void)
      {
        inf(DTR("Main method started."));
        stimateSalinity();

        while (!stopping())
        {
          inf(DTR("consensus loop in main method started."));

          Delay::wait(1.0);
          try
          {
            if (IO::Poll::poll(m_sock, 1.0))
              readMessage(m_sock);
          }
          catch (...)
          {
          }
        }
      }
    }; // namespace Consensus
  }    // namespace Consensus
} // namespace Transports

DUNE_TASK
