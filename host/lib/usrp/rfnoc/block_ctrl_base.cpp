//
// Copyright 2014 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

// This file contains the block control functions for block controller classes.
// See block_ctrl_base_factory.cpp for discovery and factory functions.

#include <boost/format.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/utils/log.hpp>
#include <uhd/usrp/rfnoc/block_ctrl_base.hpp>
#include <uhd/usrp/rfnoc/constants.hpp>

using namespace uhd;
using namespace uhd::rfnoc;

/***********************************************************************
 * Helpers
 **********************************************************************/
//! Convert register to a peek/poke compatible address
inline boost::uint32_t _sr_to_addr(boost::uint32_t reg) { return reg * 4; };
inline boost::uint32_t _sr_to_addr64(boost::uint32_t reg) { return reg * 8; }; // for peek64

/***********************************************************************
 * Structors
 **********************************************************************/
block_ctrl_base::block_ctrl_base(
        const make_args_t &make_args
) : _ctrl_iface(make_args.ctrl_iface),
    _tree(make_args.tree),
    _transport_is_big_endian(make_args.is_big_endian),
    _ctrl_sid(make_args.ctrl_sid)
{
    UHD_MSG(status) << "block_ctrl_base()" << std::endl;

    /*** Identify this block (NoC-ID, block-ID, and block definition) *******/
    // Read NoC-ID (name is passed in through make_args):
    boost::uint64_t noc_id = sr_read64(SR_READBACK_REG_ID);
    _block_def = blockdef::make_from_noc_id(noc_id);
    if (_block_def) UHD_MSG(status) <<  "Found valid blockdef" << std::endl;
    if (not _block_def)
        _block_def = blockdef::make_from_noc_id(DEFAULT_NOC_ID);
    UHD_ASSERT_THROW(_block_def);
    // For the block ID, we start with block count 0 and increase until
    // we get a block ID that's not already registered:
    _block_id.set(make_args.device_index, make_args.block_name, 0);
    while (_tree->exists("xbar/" + _block_id.get_local())) {
        _block_id++;
    }
    UHD_MSG(status)
        << "NOC ID: " << str(boost::format("0x%016X  ") % noc_id)
        << "Block ID: " << _block_id << std::endl;

    /*** Initialize property tree *******************************************/
    _root_path = "xbar/" + _block_id.get_local();
    _tree->create<boost::uint64_t>(_root_path / "noc_id").set(noc_id);

    // Read buffer sizes (also, identifies which ports may receive connections)
    std::vector<size_t> buf_sizes(16, 0);
    for (size_t port_offset = 0; port_offset < 16; port_offset += 8) {
        // FIXME: Eventually need to implement per block port buffers
        // settingsbus_reg_t reg =
        //     (port_offset == 0) ? SR_READBACK_REG_BUFFALLOC0 : SR_READBACK_REG_BUFFALLOC1;
        settingsbus_reg_t reg = SR_READBACK_REG_BUFFALLOC0;
        boost::uint64_t value = sr_read64(reg);
        for (size_t i = 0; i < 8; i++) {
            // FIXME: See above
            // size_t buf_size_log2 = (value >> (i * 8)) & 0xFF; // Buffer size in x = log2(lines)
            size_t buf_size_log2 = value & 0xFF;
            size_t buf_size_bytes = BYTES_PER_LINE * (1 << buf_size_log2); // Bytes == 8 * 2^x
            buf_sizes[i + port_offset] = BYTES_PER_LINE * (1 << buf_size_log2); // Bytes == 8 * 2^x
            _tree->create<size_t>(
                    _root_path / str(boost::format("input_buffer_size/%d") % size_t(i + port_offset))
            ).set(buf_size_bytes);
        }
    }

    /*** Init I/O signature *************************************************/
    _init_stream_sigs("input_sig",  _block_def->get_input_ports());
    _init_stream_sigs("output_sig", _block_def->get_output_ports());
    // TODO: It's possible that the number of input sigs doesn't match the
    // number of input buffers. We should probably warn about that or
    // something.
}

block_ctrl_base::~block_ctrl_base()
{
    // nop
}

void block_ctrl_base::_init_stream_sigs(
        const std::string &sig_node,
        blockdef::ports_t ports
) {
    for (size_t i = 0; i < ports.size(); i++) {
        std::string type = ports[i].types.empty() ? stream_sig_t::PASSTHRU_TYPE : ports[i].types[0];
        size_t packet_size = DEFAULT_PACKET_SIZE;
        // TODO don't ignore this
        size_t vlen = 0;
        // TODO name, optional?
        UHD_MSG(status) << "Adding stream signature at " << (_root_path / sig_node / i)
            << ": type = " << type << " packet_size = " << packet_size << " vlen = " << vlen << std::endl;
        _tree->create<stream_sig_t>(_root_path / sig_node / i)
            .set(stream_sig_t(type, vlen, packet_size));
    }
}

/***********************************************************************
 * FPGA control & communication
 **********************************************************************/
void block_ctrl_base::sr_write(const boost::uint32_t reg, const boost::uint32_t data)
{
    UHD_MSG(status) << "  ";
    UHD_RFNOC_BLOCK_TRACE() << boost::format("sr_write(%d, %08X)") % reg % data << std::endl;
    try {
        _ctrl_iface->poke32(_sr_to_addr(reg), data);
    }
    catch(const std::exception &ex) {
        throw uhd::io_error(str(boost::format("[%s] sr_write() failed: %s") % get_block_id().get() % ex.what()));
    }
}

boost::uint64_t block_ctrl_base::sr_read64(const settingsbus_reg_t reg)
{
    try {
        return _ctrl_iface->peek64(_sr_to_addr64(reg));
    }
    catch(const std::exception &ex) {
        throw uhd::io_error(str(boost::format("[%s] sr_read64() failed: %s") % get_block_id().get() % ex.what()));
    }
}

boost::uint32_t block_ctrl_base::sr_read32(const settingsbus_reg_t reg) {
    try {
        return _ctrl_iface->peek32(_sr_to_addr(reg));
    }
    catch(const std::exception &ex) {
        throw uhd::io_error(str(boost::format("[%s] sr_read32() failed: %s") % get_block_id().get() % ex.what()));
    }
}

boost::uint64_t block_ctrl_base::user_reg_read64(const boost::uint32_t addr)
{
    try {
        // Set readback register address
        sr_write(SR_READBACK_ADDR, addr);
        // Read readback register via RFNoC
        return sr_read64(SR_READBACK_REG_USER);
    }
    catch(const std::exception &ex) {
        throw uhd::io_error(str(boost::format("%s user_reg_read64() failed: %s") % get_block_id().get() % ex.what()));
    }
}

boost::uint32_t block_ctrl_base::user_reg_read32(const boost::uint32_t addr)
{
    try {
        // Set readback register address
        sr_write(SR_READBACK_ADDR, addr);
        // Read readback register via RFNoC
        return sr_read32(SR_READBACK_REG_USER);
    }
    catch(const std::exception &ex) {
        throw uhd::io_error(str(boost::format("[%s] user_reg_read32() failed: %s") % get_block_id().get() % ex.what()));
    }
}

void block_ctrl_base::clear()
{
    UHD_RFNOC_BLOCK_TRACE() << "block_ctrl_base::clear() " << std::endl;
    // Call parent...
    node_ctrl_base::clear();
    // TODO: Reset stream signatures to defaults from block definition
    // Call block-specific reset
    // ...then child
    _clear();
}

boost::uint32_t block_ctrl_base::get_address(size_t block_port) {
    return (_ctrl_sid.get_dst() & 0xFFF0) | (block_port & 0xF);
}

/***********************************************************************
 * Hooks & Derivables
 **********************************************************************/
void block_ctrl_base::_clear()
{
    UHD_RFNOC_BLOCK_TRACE() << "block_ctrl_base::_clear() " << std::endl;
    sr_write(SR_FLOW_CTRL_CLR_SEQ, 0x00C1EA12); // 'CLEAR', but we can write anything, really
}

// vim: sw=4 et:
