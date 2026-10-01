/*
 * Copyright (c) 2009 IITP RAS
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Based on
 *      NS-2 AODV model developed by the CMU/MONARCH group and optimized and
 *      tuned by Samir Das and Mahesh Marina, University of Cincinnati;
 *
 *      AODV-UU implementation by Erik Nordström of Uppsala University
 *      https://web.archive.org/web/20100527072022/http://core.it.uu.se/core/index.php/AODV-UU
 *
 * Authors: Elena Buchatskaia <borovkovaes@iitp.ru>
 *          Pavel Boyko <boyko@iitp.ru>
 */
/*
 * ---------------------------------------------------------------------------
 * SCR FORK NOTICE
 *
 * This file is a fork of the ns-3.46.1 AODV module (src/aodv), taken verbatim
 * and then modified for the Service-Confirmed Repair (SCR) study. The upstream
 * copyright, licence and authorship above are those of the original AODV
 * implementation and are reproduced unchanged.
 *
 * Upstream source : ns-3.46.1, commit 51387bce7e5f5c57aa080612a4ed690bf33d0c92
 * Fork created    : 2026-09-07
 * Fork namespace  : ns3::scr  (upstream ns3::aodv remains present and untouched,
 *                   so that AODV-STOCK is a genuine native reference)
 *
 * SCR-specific modifications are marked with "SCR:" comments. Where no such
 * marker appears, the logic is upstream AODV behaviour.
 * ---------------------------------------------------------------------------
 */

#ifndef SCRPACKET_H
#define SCRPACKET_H

#include "ns3/enum.h"
#include "ns3/header.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"

#include <iostream>
#include <map>
#include <vector>

namespace ns3
{
namespace scr
{

/**
 * @ingroup scr
 * @brief MessageType enumeration
 */
enum MessageType
{
    SCRTYPE_RREQ = 1,    //!< SCRTYPE_RREQ
    SCRTYPE_RREP = 2,    //!< SCRTYPE_RREP
    SCRTYPE_RERR = 3,    //!< SCRTYPE_RERR
    SCRTYPE_RREP_ACK = 4 //!< SCRTYPE_RREP_ACK
};

/**
 * @ingroup scr
 * @brief SCR types
 */
class TypeHeader : public Header
{
  public:
    /**
     * constructor
     * @param t the SCR RREQ type
     */
    TypeHeader(MessageType t = SCRTYPE_RREQ);

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    /**
     * @returns the type
     */
    MessageType Get() const
    {
        return m_type;
    }

    /**
     * Check that type if valid
     * @returns true if the type is valid
     */
    bool IsValid() const
    {
        return m_valid;
    }

    /**
     * @brief Comparison operator
     * @param o header to compare
     * @return true if the headers are equal
     */
    bool operator==(const TypeHeader& o) const;

  private:
    MessageType m_type; ///< type of the message
    bool m_valid;       ///< Indicates if the message is valid
};

/**
 * @brief Stream output operator
 * @param os output stream
 * @param h the TypeHeader
 * @return updated stream
 */
std::ostream& operator<<(std::ostream& os, const TypeHeader& h);

/**
* @ingroup scr
* @brief   Route Request (RREQ) Message Format
  \verbatim
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |     Type      |J|R|G|D|U|   Reserved          |   Hop Count   |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                            RREQ ID                            |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                    Destination IP Address                     |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                  Destination Sequence Number                  |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                    Originator IP Address                      |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                  Originator Sequence Number                   |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \endverbatim
*/
class RreqHeader : public Header
{
  public:
    /**
     * constructor
     *
     * @param flags the message flags (0)
     * @param reserved the reserved bits (0)
     * @param hopCount the hop count
     * @param requestID the request ID
     * @param dst the destination IP address
     * @param dstSeqNo the destination sequence number
     * @param origin the origin IP address
     * @param originSeqNo the origin sequence number
     */
    RreqHeader(uint8_t flags = 0,
               uint8_t reserved = 0,
               uint8_t hopCount = 0,
               uint32_t requestID = 0,
               Ipv4Address dst = Ipv4Address(),
               uint32_t dstSeqNo = 0,
               Ipv4Address origin = Ipv4Address(),
               uint32_t originSeqNo = 0);

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    // Fields
    /**
     * @brief Set the hop count
     * @param count the hop count
     */
    void SetHopCount(uint8_t count)
    {
        m_hopCount = count;
    }

    /**
     * @brief Get the hop count
     * @return the hop count
     */
    uint8_t GetHopCount() const
    {
        return m_hopCount;
    }

    /**
     * @brief Set the request ID
     * @param id the request ID
     */
    void SetId(uint32_t id)
    {
        m_requestID = id;
    }

    /**
     * @brief CLAF-AODV (R057): path quality metric in the reserved byte, as in
     * Safari et al. (Fig. 4), quantised to 1/255. Only CLAF-AODV writes it, so
     * every other mode keeps the byte at 0 and its RREQs are unchanged.
     * @param q path quality in [0,1]
     */
    void SetPathQuality(double q)
    {
        const double c = q < 0.0 ? 0.0 : (q > 1.0 ? 1.0 : q);
        m_reserved = static_cast<uint8_t>(c * 255.0 + 0.5);
    }

    /**
     * @brief CLAF-AODV (R057): path quality carried in the reserved byte.
     * @return path quality in [0,1]
     */
    double GetPathQuality() const
    {
        return m_reserved / 255.0;
    }

    /**
     * @brief Get the request ID
     * @return the request ID
     */
    uint32_t GetId() const
    {
        return m_requestID;
    }

    /**
     * @brief Set the destination address
     * @param a the destination address
     */
    void SetDst(Ipv4Address a)
    {
        m_dst = a;
    }

    /**
     * @brief Get the destination address
     * @return the destination address
     */
    Ipv4Address GetDst() const
    {
        return m_dst;
    }

    /**
     * @brief Set the destination sequence number
     * @param s the destination sequence number
     */
    void SetDstSeqno(uint32_t s)
    {
        m_dstSeqNo = s;
    }

    /**
     * @brief Get the destination sequence number
     * @return the destination sequence number
     */
    uint32_t GetDstSeqno() const
    {
        return m_dstSeqNo;
    }

    /**
     * @brief Set the origin address
     * @param a the origin address
     */
    void SetOrigin(Ipv4Address a)
    {
        m_origin = a;
    }

    /**
     * @brief Get the origin address
     * @return the origin address
     */
    Ipv4Address GetOrigin() const
    {
        return m_origin;
    }

    /**
     * @brief Set the origin sequence number
     * @param s the origin sequence number
     */
    void SetOriginSeqno(uint32_t s)
    {
        m_originSeqNo = s;
    }

    /**
     * @brief Get the origin sequence number
     * @return the origin sequence number
     */
    uint32_t GetOriginSeqno() const
    {
        return m_originSeqNo;
    }

    // Flags
    /**
     * @brief Set the gratuitous RREP flag
     * @param f the gratuitous RREP flag
     */
    void SetGratuitousRrep(bool f);
    /**
     * @brief Get the gratuitous RREP flag
     * @return the gratuitous RREP flag
     */
    bool GetGratuitousRrep() const;
    /**
     * @brief Set the Destination only flag
     * @param f the Destination only flag
     */
    void SetDestinationOnly(bool f);
    /**
     * @brief Get the Destination only flag
     * @return the Destination only flag
     */
    bool GetDestinationOnly() const;
    /**
     * @brief Set the unknown sequence number flag
     * @param f the unknown sequence number flag
     */
    void SetUnknownSeqno(bool f);
    /**
     * @brief Get the unknown sequence number flag
     * @return the unknown sequence number flag
     */
    bool GetUnknownSeqno() const;

    /**
     * @brief Comparison operator
     * @param o RREQ header to compare
     * @return true if the RREQ headers are equal
     */
    bool operator==(const RreqHeader& o) const;

  private:
    uint8_t m_flags;        ///< |J|R|G|D|U| bit flags, see RFC
    uint8_t m_reserved;     ///< Not used (must be 0), except by CLAF-AODV (path quality, R057)
    uint8_t m_hopCount;     ///< Hop Count
    uint32_t m_requestID;   ///< RREQ ID
    Ipv4Address m_dst;      ///< Destination IP Address
    uint32_t m_dstSeqNo;    ///< Destination Sequence Number
    Ipv4Address m_origin;   ///< Originator IP Address
    uint32_t m_originSeqNo; ///< Source Sequence Number
};

/**
 * @brief Stream output operator
 * @param os output stream
 * @return updated stream
 */
std::ostream& operator<<(std::ostream& os, const RreqHeader&);

/**
* @ingroup scr
* @brief Route Reply (RREP) Message Format
  \verbatim
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |     Type      |R|A|    Reserved     |Prefix Sz|   Hop Count   |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                     Destination IP address                    |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                  Destination Sequence Number                  |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                    Originator IP address                      |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                           Lifetime                            |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \endverbatim
*/
class RrepHeader : public Header
{
  public:
    /**
     * constructor
     *
     * @param prefixSize the prefix size (0)
     * @param hopCount the hop count (0)
     * @param dst the destination IP address
     * @param dstSeqNo the destination sequence number
     * @param origin the origin IP address
     * @param lifetime the lifetime
     */
    RrepHeader(uint8_t prefixSize = 0,
               uint8_t hopCount = 0,
               Ipv4Address dst = Ipv4Address(),
               uint32_t dstSeqNo = 0,
               Ipv4Address origin = Ipv4Address(),
               Time lifetime = MilliSeconds(0));
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    // Fields
    /**
     * @brief Set the hop count
     * @param count the hop count
     */
    void SetHopCount(uint8_t count)
    {
        m_hopCount = count;
    }

    /**
     * @brief Get the hop count
     * @return the hop count
     */
    uint8_t GetHopCount() const
    {
        return m_hopCount;
    }

    /**
     * @brief Set the destination address
     * @param a the destination address
     */
    void SetDst(Ipv4Address a)
    {
        m_dst = a;
    }

    /**
     * @brief Get the destination address
     * @return the destination address
     */
    Ipv4Address GetDst() const
    {
        return m_dst;
    }

    /**
     * @brief Set the destination sequence number
     * @param s the destination sequence number
     */
    void SetDstSeqno(uint32_t s)
    {
        m_dstSeqNo = s;
    }

    /**
     * @brief Get the destination sequence number
     * @return the destination sequence number
     */
    uint32_t GetDstSeqno() const
    {
        return m_dstSeqNo;
    }

    /**
     * @brief Set the origin address
     * @param a the origin address
     */
    void SetOrigin(Ipv4Address a)
    {
        m_origin = a;
    }

    /**
     * @brief Get the origin address
     * @return the origin address
     */
    Ipv4Address GetOrigin() const
    {
        return m_origin;
    }

    /**
     * @brief Set the lifetime
     * @param t the lifetime
     */
    void SetLifeTime(Time t);
    /**
     * @brief Get the lifetime
     * @return the lifetime
     */
    Time GetLifeTime() const;

    // Flags
    /**
     * @brief Set the ack required flag
     * @param f the ack required flag
     */
    void SetAckRequired(bool f);
    /**
     * @brief get the ack required flag
     * @return the ack required flag
     */
    bool GetAckRequired() const;
    /**
     * @brief Set the prefix size
     * @param sz the prefix size
     */
    void SetPrefixSize(uint8_t sz);
    /**
     * @brief Set the prefix size
     * @return the prefix size
     */
    uint8_t GetPrefixSize() const;

    /**
     * Configure RREP to be a Hello message
     *
     * @param src the source IP address
     * @param srcSeqNo the source sequence number
     * @param lifetime the lifetime of the message
     */
    void SetHello(Ipv4Address src, uint32_t srcSeqNo, Time lifetime);

    /**
     * @brief Comparison operator
     * @param o RREP header to compare
     * @return true if the RREP headers are equal
     */
    bool operator==(const RrepHeader& o) const;

  private:
    uint8_t m_flags;      ///< A - acknowledgment required flag
    uint8_t m_prefixSize; ///< Prefix Size
    uint8_t m_hopCount;   ///< Hop Count
    Ipv4Address m_dst;    ///< Destination IP Address
    uint32_t m_dstSeqNo;  ///< Destination Sequence Number
    Ipv4Address m_origin; ///< Source IP Address
    uint32_t m_lifeTime;  ///< Lifetime (in milliseconds)
};

/**
 * @brief Stream output operator
 * @param os output stream
 * @return updated stream
 */
std::ostream& operator<<(std::ostream& os, const RrepHeader&);

/**
* @ingroup scr
* @brief Route Reply Acknowledgment (RREP-ACK) Message Format
  \verbatim
  0                   1
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |     Type      |   Reserved    |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \endverbatim
*/
class RrepAckHeader : public Header
{
  public:
    /// constructor
    RrepAckHeader();

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    /**
     * @brief Comparison operator
     * @param o RREP header to compare
     * @return true if the RREQ headers are equal
     */
    bool operator==(const RrepAckHeader& o) const;

  private:
    uint8_t m_reserved; ///< Not used (must be 0)
};

/**
 * @brief Stream output operator
 * @param os output stream
 * @return updated stream
 */
std::ostream& operator<<(std::ostream& os, const RrepAckHeader&);

/**
* @ingroup scr
* @brief Route Error (RERR) Message Format
  \verbatim
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |     Type      |N|          Reserved           |   DestCount   |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |            Unreachable Destination IP Address (1)             |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |         Unreachable Destination Sequence Number (1)           |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-|
  |  Additional Unreachable Destination IP Addresses (if needed)  |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |Additional Unreachable Destination Sequence Numbers (if needed)|
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  \endverbatim
*/
class RerrHeader : public Header
{
  public:
    /// constructor
    RerrHeader();

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator i) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    // No delete flag
    /**
     * @brief Set the no delete flag
     * @param f the no delete flag
     */
    void SetNoDelete(bool f);
    /**
     * @brief Get the no delete flag
     * @return the no delete flag
     */
    bool GetNoDelete() const;
    /**
     * @brief CARD (R053): mark the break as a topology change.
     *
     * Carried in bit 1 of the flag byte, which RFC 3561 leaves reserved and no
     * other mode sets, so the header size and every non-CARD RERR are unchanged.
     * @param f true if the detecting node judged the break a topology change
     */
    void SetTopologyChange(bool f);
    /**
     * @brief CARD (R053): whether the break was judged a topology change
     * @return the topology-change bit
     */
    bool GetTopologyChange() const;

    /**
     * @brief Add unreachable node address and its sequence number in RERR header
     * @param dst unreachable IPv4 address
     * @param seqNo unreachable sequence number
     * @return false if we already added maximum possible number of unreachable destinations
     */
    bool AddUnDestination(Ipv4Address dst, uint32_t seqNo);
    /**
     * @brief Delete pair (address + sequence number) from REER header, if the number of unreachable
     * destinations > 0
     * @param un unreachable pair (address + sequence number)
     * @return true on success
     */
    bool RemoveUnDestination(std::pair<Ipv4Address, uint32_t>& un);
    /// Clear header
    void Clear();

    /**
     * @returns number of unreachable destinations in RERR message
     */
    uint8_t GetDestCount() const
    {
        return (uint8_t)m_unreachableDstSeqNo.size();
    }

    /**
     * @brief Comparison operator
     * @param o RERR header to compare
     * @return true if the RERR headers are equal
     */
    bool operator==(const RerrHeader& o) const;

  private:
    uint8_t m_flag;     ///< No delete flag
    uint8_t m_reserved; ///< Not used (must be 0)

    /// List of Unreachable destination: IP addresses and sequence numbers
    std::map<Ipv4Address, uint32_t> m_unreachableDstSeqNo;
};

/**
 * @brief Stream output operator
 * @param os output stream
 * @return updated stream
 */
std::ostream& operator<<(std::ostream& os, const RerrHeader&);

/**
 * @ingroup scr
 * @brief TAAODV (R058) extension, appended after the HELLO, RREQ or RREP header.
 *
 * Li et al., IEEE T-ITS 26 (2025) 17255-17268, add fields to all three
 * messages (Sec. V-C) without giving a layout; this is ours. Only TAAODV mode
 * sends or parses it, so every other mode's packets are unchanged.
 * \verbatim
   kind (1)   1 = HELLO, 2 = RREQ, 3 = RREP
   HELLO: x, y, vx, vy (4 x float32), n (1), n x neighbour address (4)
   RREQ:  accumulated pheromone (float32), k (1), k x selected forwarder (4)
   RREP:  accumulated pheromone (float32)
   \endverbatim
 */
class TaaodvExtHeader : public Header
{
  public:
    /// Message the extension belongs to.
    enum Kind : uint8_t
    {
        TAA_HELLO = 1,
        TAA_RREQ = 2,
        TAA_RREP = 3,
    };

    TaaodvExtHeader();

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    Kind m_kind{TAA_HELLO};                //!< message kind
    float m_x{0}, m_y{0}, m_vx{0}, m_vy{0}; //!< HELLO: sender position (m) and velocity (m/s)
    float m_pheromone{0};                  //!< RREQ, RREP: accumulated pheromone (Eq. 26)
    std::vector<Ipv4Address> m_addrs;      //!< HELLO: neighbours; RREQ: selected forwarders
};

} // namespace scr
} // namespace ns3

#endif /* SCRPACKET_H */
