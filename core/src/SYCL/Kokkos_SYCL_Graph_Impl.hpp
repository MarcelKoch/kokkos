//@HEADER
// ************************************************************************
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
// Part of Kokkos, under the Apache License v2.0 with LLVM Exceptions.
// See https://kokkos.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//@HEADER

#ifndef KOKKOS_SYCL_GRAPH_IMPL_HPP
#define KOKKOS_SYCL_GRAPH_IMPL_HPP

#include <Kokkos_Macros.hpp>

#include <Kokkos_Graph_fwd.hpp>

#include <impl/Kokkos_GraphImpl.hpp>
#include <impl/Kokkos_GraphNodeImpl.hpp>

#include <SYCL/Kokkos_SYCL_GraphNodeKernel.hpp>

#include <optional>

namespace Kokkos {
namespace Impl {
template <>
class GraphImpl<Kokkos::SYCL> {
 public:
  using node_details_t = GraphNodeBackendSpecificDetails<Kokkos::SYCL>;
  using root_node_impl_t =
      GraphNodeImpl<Kokkos::SYCL, Kokkos::Experimental::TypeErasedTag,
                    Kokkos::Experimental::TypeErasedTag>;
  using aggregate_kernel_impl_t = SYCLGraphNodeAggregateKernel;
  using aggregate_node_impl_t =
      GraphNodeImpl<Kokkos::SYCL, aggregate_kernel_impl_t,
                    Kokkos::Experimental::TypeErasedTag>;

  // Not movable or copyable; it spends its whole life as a shared_ptr in the
  // Graph object.
  GraphImpl()                            = delete;
  GraphImpl(GraphImpl const&)            = delete;
  GraphImpl(GraphImpl&&)                 = delete;
  GraphImpl& operator=(GraphImpl const&) = delete;
  GraphImpl& operator=(GraphImpl&&)      = delete;

  ~GraphImpl();

  explicit GraphImpl(Kokkos::SYCL instance);

  void add_node(std::shared_ptr<aggregate_node_impl_t> const& arg_node_ptr);

  template <class NodeImpl>
  void add_node(std::shared_ptr<NodeImpl> const& arg_node_ptr);

  template <class NodeImplPtr, class PredecessorRef>
  void add_predecessor(NodeImplPtr arg_node_ptr, PredecessorRef arg_pred_ref);

  void submit();

  Kokkos::SYCL const& get_execution_space() const noexcept;

  auto create_root_node_ptr();

  template <class... PredecessorRefs>
  auto create_aggregate_ptr(PredecessorRefs&&...);

  void instantiate() {
    KOKKOS_EXPECTS(!m_graph_exec.has_value());
    m_graph_exec = m_graph.finalize();
  }

 private:
  Kokkos::SYCL m_execution_space;
  sycl::ext::oneapi::experimental::command_graph<
      sycl::ext::oneapi::experimental::graph_state::modifiable>
      m_graph;
  std::optional<sycl::ext::oneapi::experimental::command_graph<
      sycl::ext::oneapi::experimental::graph_state::executable>>
      m_graph_exec;
};

inline GraphImpl<Kokkos::SYCL>::~GraphImpl() {
  m_execution_space.fence("Kokkos::GraphImpl::~GraphImpl: Graph Destruction");
}

inline GraphImpl<Kokkos::SYCL>::GraphImpl(Kokkos::SYCL instance)
    : m_execution_space(std::move(instance)),
      m_graph(m_execution_space.sycl_queue().get_context(),
              m_execution_space.sycl_queue().get_device()) {}

inline void GraphImpl<Kokkos::SYCL>::add_node(
    std::shared_ptr<aggregate_node_impl_t> const& arg_node_ptr) {
  // add an empty node that needs to be set up before finalizing the graph
  arg_node_ptr->node_details_t::node = m_graph.add();
}

// Requires NodeImplPtr is a shared_ptr to specialization of GraphNodeImpl
// Also requires that the kernel has the graph node tag in its policy
template <class NodeImpl>
inline void GraphImpl<Kokkos::SYCL>::add_node(
    std::shared_ptr<NodeImpl> const& arg_node_ptr) {
  static_assert(NodeImpl::kernel_type::Policy::is_graph_kernel::value);
  KOKKOS_EXPECTS(arg_node_ptr);
  // The Kernel launch from the execute() method has been shimmed to insert
  // the node into the graph
  auto& kernel = arg_node_ptr->get_kernel();
  auto& node   = static_cast<node_details_t*>(arg_node_ptr.get())->node;
  KOKKOS_EXPECTS(!node);
  kernel.set_sycl_graph_ptr(&m_graph);
  kernel.set_sycl_graph_node_ptr(&node);
  kernel.execute();
  KOKKOS_ENSURES(node);
}

// Requires PredecessorRef is a specialization of GraphNodeRef that has
// already been added to this graph and NodeImpl is a specialization of
// GraphNodeImpl that has already been added to this graph.
template <class NodeImplPtr, class PredecessorRef>
inline void GraphImpl<Kokkos::SYCL>::add_predecessor(
    NodeImplPtr arg_node_ptr, PredecessorRef arg_pred_ref) {
  KOKKOS_EXPECTS(arg_node_ptr);
  auto pred_ptr = GraphAccess::get_node_ptr(arg_pred_ref);
  KOKKOS_EXPECTS(pred_ptr);

  auto& pred_node = pred_ptr->node_details_t::node;
  KOKKOS_EXPECTS(pred_node);

  auto& node = arg_node_ptr->node_details_t::node;
  KOKKOS_EXPECTS(node);

  m_graph.make_edge(*pred_node, *node);
}

inline void GraphImpl<Kokkos::SYCL>::submit() {
  if (!m_graph_exec) {
    instantiate();
  }
  m_execution_space.sycl_queue().ext_oneapi_graph(*m_graph_exec);
}

inline Kokkos::SYCL const& GraphImpl<Kokkos::SYCL>::get_execution_space()
    const noexcept {
  return m_execution_space;
}

inline auto GraphImpl<Kokkos::SYCL>::create_root_node_ptr() {
  KOKKOS_EXPECTS(!m_graph_exec);
  auto rv = std::make_shared<root_node_impl_t>(get_execution_space(),
                                               _graph_node_is_root_ctor_tag{});
  rv->node_details_t::node = m_graph.add();
  return rv;
}

template <class... PredecessorRefs>
inline auto GraphImpl<Kokkos::SYCL>::create_aggregate_ptr(
    PredecessorRefs&&...) {
  // The attachment to predecessors, which is all we really need, happens
  // in the generic layer, which calls through to add_predecessor for
  // each predecessor ref, so all we need to do here is create the (trivial)
  // aggregate node.
  return std::make_shared<aggregate_node_impl_t>(m_execution_space,
                                                 _graph_node_kernel_ctor_tag{},
                                                 aggregate_kernel_impl_t{});
}
}  // namespace Impl
}  // namespace Kokkos

#endif
