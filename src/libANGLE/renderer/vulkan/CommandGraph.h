//
// Copyright 2017 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CommandGraph:
//    Deferred work constructed by GL calls, that will later be flushed to Vulkan.
//

#ifndef LIBANGLE_RENDERER_VULKAN_COMMAND_GRAPH_H_
#define LIBANGLE_RENDERER_VULKAN_COMMAND_GRAPH_H_

#include "libANGLE/renderer/vulkan/SecondaryCommandBuffer.h"
#include "libANGLE/renderer/vulkan/vk_cache_utils.h"

namespace rx
{

namespace vk
{
class CommandGraph;

enum class VisitedState
{
    Unvisited,
    Ready,
    Visited,
};

enum class CommandGraphResourceType
{
    Buffer,
    Framebuffer,
    Image,
    Query,
    Dispatcher,
    // Transform feedback queries could be handled entirely on the CPU (if not using
    // VK_EXT_transform_feedback), but still need to generate a command graph barrier node.
    EmulatedQuery,
    FenceSync,
    GraphBarrier,
    DebugMarker,
    HostAvailabilityOperation,
};

// Certain functionality cannot be put in secondary command buffers, so they are special-cased in
// the node.
enum class CommandGraphNodeFunction
{
    Generic,
    BeginQuery,
    EndQuery,
    WriteTimestamp,
    BeginTransformFeedbackQuery,
    EndTransformFeedbackQuery,
    SetFenceSync,
    WaitFenceSync,
    GraphBarrier,
    InsertDebugMarker,
    PushDebugMarker,
    PopDebugMarker,
    HostAvailabilityOperation,
};

// Receives notifications when a render pass command buffer is no longer able to record. Can be
// used with inheritance. Faster than using an interface class since it has inlined methods. Could
// be used with composition by adding a getCommandBuffer method.
class RenderPassOwner
{
  public:
    RenderPassOwner() = default;
    virtual ~RenderPassOwner() {}

    ANGLE_INLINE void onRenderPassFinished() { mRenderPassCommandBuffer = nullptr; }

  protected:
    CommandBuffer *mRenderPassCommandBuffer = nullptr;
};

// Only used internally in the command graph. Kept in the header for better inlining performance.
class CommandGraphNode final : angle::NonCopyable
{
  public:
    CommandGraphNode(CommandGraphNodeFunction function, angle::PoolAllocator *poolAllocator);
    ~CommandGraphNode();

    // Immutable queries for when we're walking the commands tree.
    CommandBuffer *getOutsideRenderPassCommands()
    {
        ASSERT(!mHasChildren);
        return &mOutsideRenderPassCommands;
    }

    CommandBuffer *getInsideRenderPassCommands()
    {
        ASSERT(!mHasChildren);
        return &mInsideRenderPassCommands;
    }

    // For outside the render pass (copies, transitions, etc).
    angle::Result beginOutsideRenderPassRecording(ContextVk *context,
                                                  const CommandPool &commandPool,
                                                  CommandBuffer **commandsOut);

    // For rendering commands (draws).
    angle::Result beginInsideRenderPassRecording(ContextVk *context, CommandBuffer **commandsOut);

    // storeRenderPassInfo and append*RenderTarget store info relevant to the RenderPass.
    void storeRenderPassInfo(const Framebuffer &framebuffer,
                             const gl::Rectangle renderArea,
                             const vk::RenderPassDesc &renderPassDesc,
                             const AttachmentOpsArray &renderPassAttachmentOps,
                             const std::vector<VkClearValue> &clearValues);

    void clearRenderPassColorAttachment(size_t attachmentIndex, const VkClearColorValue &clearValue)
    {
        mRenderPassAttachmentOps[attachmentIndex].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        mRenderPassClearValues[attachmentIndex].color    = clearValue;
    }

    void clearRenderPassDepthAttachment(size_t attachmentIndex, float depth)
    {
        mRenderPassAttachmentOps[attachmentIndex].loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
        mRenderPassClearValues[attachmentIndex].depthStencil.depth = depth;
    }

    void clearRenderPassStencilAttachment(size_t attachmentIndex, uint32_t stencil)
    {
        mRenderPassAttachmentOps[attachmentIndex].stencilLoadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        mRenderPassClearValues[attachmentIndex].depthStencil.stencil = stencil;
    }

    void invalidateRenderPassColorAttachment(size_t attachmentIndex)
    {
        mRenderPassAttachmentOps[attachmentIndex].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    void invalidateRenderPassDepthAttachment(size_t attachmentIndex)
    {
        mRenderPassAttachmentOps[attachmentIndex].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    void invalidateRenderPassStencilAttachment(size_t attachmentIndex)
    {
        mRenderPassAttachmentOps[attachmentIndex].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    // Dependency commands order node execution in the command graph.
    // Once a node has commands that must happen after it, recording is stopped and the node is
    // frozen forever.
    static void SetHappensBeforeDependency(CommandGraphNode *beforeNode,
                                           CommandGraphNode *afterNode)
    {
        ASSERT(beforeNode != afterNode && !beforeNode->isChildOf(afterNode));
        afterNode->mParents.emplace_back(beforeNode);
        beforeNode->setHasChildren();
    }

    static void SetHappensBeforeDependencies(CommandGraphNode **beforeNodes,
                                             size_t beforeNodesCount,
                                             CommandGraphNode *afterNode);

    static void SetHappensBeforeDependencies(CommandGraphNode *beforeNode,
                                             CommandGraphNode **afterNodes,
                                             size_t afterNodesCount);

    bool hasParents() const;
    bool hasChildren() const { return mHasChildren; }

    // Commands for traversing the node on a flush operation.
    VisitedState visitedState() const;
    void visitParents(std::vector<CommandGraphNode *> *stack);
    angle::Result visitAndExecute(Context *context,
                                  Serial serial,
                                  RenderPassCache *renderPassCache,
                                  PrimaryCommandBuffer *primaryCommandBuffer);

    // Only used in the command graph diagnostics.
    const std::vector<CommandGraphNode *> &getParentsForDiagnostics() const;
    void setDiagnosticInfo(CommandGraphResourceType resourceType, uintptr_t resourceID);

    CommandGraphResourceType getResourceTypeForDiagnostics() const { return mResourceType; }
    uintptr_t getResourceIDForDiagnostics() const { return mResourceID; }
    bool hasDiagnosticID() const;
    std::string dumpCommandsForDiagnostics(const char *separator) const;
    void getMemoryUsageStatsForDiagnostics(size_t *usedMemoryOut, size_t *allocatedMemoryOut) const;

    const gl::Rectangle &getRenderPassRenderArea() const { return mRenderPassRenderArea; }

    CommandGraphNodeFunction getFunction() const { return mFunction; }

    void setQueryPool(const QueryPool *queryPool, uint32_t queryIndex);
    VkQueryPool getQueryPool() const { return mQueryPool; }
    uint32_t getQueryIndex() const { return mQueryIndex; }
    void setFenceSync(const vk::Event &event);
    void setDebugMarker(GLenum source, std::string &&marker);
    const std::string &getDebugMarker() const { return mDebugMarker; }

    ANGLE_INLINE void addGlobalMemoryBarrier(VkFlags srcAccess,
                                             VkFlags dstAccess,
                                             VkPipelineStageFlags stages)
    {
        mGlobalMemoryBarrierSrcAccess |= srcAccess;
        mGlobalMemoryBarrierDstAccess |= dstAccess;
        mGlobalMemoryBarrierStages |= stages;
    }

    // This can only be set for RenderPass nodes. Each RenderPass node can have at most one owner.
    void setRenderPassOwner(RenderPassOwner *owner)
    {
        ASSERT(mRenderPassOwner == nullptr);
        mRenderPassOwner = owner;
    }

  private:
    ANGLE_INLINE void setHasChildren()
    {
        mHasChildren = true;
        if (mRenderPassOwner)
        {
            mRenderPassOwner->onRenderPassFinished();
        }
    }

    // Used for testing only.
    bool isChildOf(CommandGraphNode *parent);

    // Only used if we need a RenderPass for these commands.
    RenderPassDesc mRenderPassDesc;
    AttachmentOpsArray mRenderPassAttachmentOps;
    Framebuffer mRenderPassFramebuffer;
    gl::Rectangle mRenderPassRenderArea;
    gl::AttachmentArray<VkClearValue> mRenderPassClearValues;

    CommandGraphNodeFunction mFunction;
    angle::PoolAllocator *mPoolAllocator;
    // Keep separate buffers for commands inside and outside a RenderPass.
    // TODO(jmadill): We might not need inside and outside RenderPass commands separate.
    CommandBuffer mOutsideRenderPassCommands;
    CommandBuffer mInsideRenderPassCommands;

    // Special-function additional data:
    // Queries:
    VkQueryPool mQueryPool;
    uint32_t mQueryIndex;
    // GLsync and EGLSync:
    VkEvent mFenceSyncEvent;
    // Debug markers:
    GLenum mDebugMarkerSource;
    std::string mDebugMarker;

    // Parents are commands that must be submitted before 'this' CommandNode can be submitted.
    std::vector<CommandGraphNode *> mParents;

    // If this is true, other commands exist that must be submitted after 'this' command.
    bool mHasChildren;

    // Used when traversing the dependency graph.
    VisitedState mVisitedState;

    // Additional diagnostic information.
    CommandGraphResourceType mResourceType;
    uintptr_t mResourceID;

    // For global memory barriers.
    VkFlags mGlobalMemoryBarrierSrcAccess;
    VkFlags mGlobalMemoryBarrierDstAccess;
    VkPipelineStageFlags mGlobalMemoryBarrierStages;

    // Render pass command buffer notifications.
    RenderPassOwner *mRenderPassOwner;
};

// Tracks how a resource is used in a command graph and in a VkQueue. The reference count indicates
// the number of times a resource is used in the graph. The serial indicates the last current use
// of a resource in the VkQueue. The reference count and serial together can determine if a
// resource is in use.
struct ResourceUse
{
    ResourceUse() = default;

    uint32_t counter = 0;
    Serial serial;
};

using SharedResourceUse = std::shared_ptr<ResourceUse>;

// This is a helper class for back-end objects used in Vk command buffers. It records a serial
// at command recording times indicating an order in the queue. We use Fences to detect when
// commands finish, and then release any unreferenced and deleted resources based on the stored
// queue serial in a special 'garbage' queue. Resources also track current read and write
// dependencies. Only one command buffer node can be writing to the Resource at a time, but many
// can be reading from it. Together the dependencies will form a command graph at submission time.
class CommandGraphResource : angle::NonCopyable
{
  public:
    virtual ~CommandGraphResource();

    // Returns true if the resource is in use by the renderer.
    bool isResourceInUse(ContextVk *contextVk) const;

    // Get the current queue serial for this resource. Used to release resources, and for
    // queries, to know if the queue they are submitted on has finished execution.
    Serial getStoredQueueSerial() const { return mUse->serial; }

    // Sets up dependency relations. 'this' resource is the resource being written to.
    void addWriteDependency(ContextVk *contextVk, CommandGraphResource *writingResource);

    // Sets up dependency relations. 'this' resource is the resource being read.
    void addReadDependency(ContextVk *contextVk, CommandGraphResource *readingResource);

    // Updates the in-use serial tracked for this resource. Will clear dependencies if the resource
    // was not used in this set of command nodes.
    // TODO(jmadill): Remove serial once migrated. http://angelbug.com/2464
    void onGraphAccess(Serial serial, CommandGraph *commandGraph);

    // Reset the current queue serial for this resource. Will clear dependencies if the resource
    // was not used in this set of command nodes.
    void resetQueueSerial();

    // Allocates a write node via getNewWriteNode and returns a started command buffer.
    // The started command buffer will render outside of a RenderPass.
    // Will append to an existing command buffer/graph node if possible.
    angle::Result recordCommands(ContextVk *context, CommandBuffer **commandBufferOut);

    // Begins a command buffer on the current graph node for in-RenderPass rendering.
    // Called from FramebufferVk::startNewRenderPass and UtilsVk functions.
    angle::Result beginRenderPass(ContextVk *contextVk,
                                  const Framebuffer &framebuffer,
                                  const gl::Rectangle &renderArea,
                                  const RenderPassDesc &renderPassDesc,
                                  const AttachmentOpsArray &renderPassAttachmentOps,
                                  const std::vector<VkClearValue> &clearValues,
                                  CommandBuffer **commandBufferOut);

    // Checks if we're in a RenderPass without children.
    bool hasStartedRenderPass() const;

    // Checks if we're in a RenderPass that encompasses renderArea, returning true if so. Updates
    // serial internally. Returns the started command buffer in commandBufferOut.
    bool appendToStartedRenderPass(Serial serial,
                                   CommandGraph *graph,
                                   const gl::Rectangle &renderArea,
                                   CommandBuffer **commandBufferOut);

    // Returns true if the render pass is started, but there are no commands yet recorded in it.
    // This is useful to know if the render pass ops can be modified.
    bool renderPassStartedButEmpty() const;

    void clearRenderPassColorAttachment(size_t attachmentIndex,
                                        const VkClearColorValue &clearValue);
    void clearRenderPassDepthAttachment(size_t attachmentIndex, float depth);
    void clearRenderPassStencilAttachment(size_t attachmentIndex, uint32_t stencil);

    void invalidateRenderPassColorAttachment(size_t attachmentIndex);
    void invalidateRenderPassDepthAttachment(size_t attachmentIndex);
    void invalidateRenderPassStencilAttachment(size_t attachmentIndex);

    // Accessor for RenderPass RenderArea.
    const gl::Rectangle &getRenderPassRenderArea() const;

    // Called when 'this' object changes, but we'd like to start a new command buffer later.
    void finishCurrentCommands(ContextVk *contextVk);

    // Store a deferred memory barrier. Will be recorded into a primary command buffer at submit.
    void addGlobalMemoryBarrier(VkFlags srcAccess, VkFlags dstAccess, VkPipelineStageFlags stages);

  protected:
    explicit CommandGraphResource(CommandGraphResourceType resourceType);

  private:
    // Returns true if this node has a current writing node with no children.
    ANGLE_INLINE bool hasChildlessWritingNode() const;

    void startNewCommands(ContextVk *contextVk);

    void onWriteImpl(ContextVk *contextVk, CommandGraphNode *writingNode);

    // Current resource lifetime.
    SharedResourceUse mUse;

    std::vector<CommandGraphNode *> mCurrentReadingNodes;

    // Current command graph writing node.
    CommandGraphNode *mCurrentWritingNode;

    // Additional diagnostic information.
    CommandGraphResourceType mResourceType;
};

// Translating OpenGL commands into Vulkan and submitting them immediately loses out on some
// of the powerful flexiblity Vulkan offers in RenderPasses. Load/Store ops can automatically
// clear RenderPass attachments, or preserve the contents. RenderPass automatic layout transitions
// can improve certain performance cases. Also, we can remove redundant RenderPass Begin and Ends
// when processing interleaved draw operations on independent Framebuffers.
//
// ANGLE's CommandGraph (and CommandGraphNode) attempt to solve these problems using deferred
// command submission. We also sometimes call this command re-ordering. A brief summary:
//
// During GL command processing, we record Vulkan commands into SecondaryCommandBuffers, which
// are stored in CommandGraphNodes, and these nodes are chained together via dependencies to
// form a directed acyclic CommandGraph. When we need to submit the CommandGraph, say during a
// SwapBuffers or ReadPixels call, we begin a primary Vulkan CommandBuffer, and walk the
// CommandGraph, starting at the most senior nodes, recording SecondaryCommandBuffers inside
// and outside RenderPasses as necessary, filled with the right load/store operations. Once
// the primary CommandBuffer has recorded all of the SecondaryCommandBuffers from all the open
// CommandGraphNodes, we submit the primary CommandBuffer to the VkQueue on the device.
//
// The Command Graph consists of an array of open Command Graph Nodes. It supports allocating new
// nodes for the graph, which are linked via dependency relation calls in CommandGraphNode, and
// also submitting the whole command graph via submitCommands.
class CommandGraph final : angle::NonCopyable
{
  public:
    explicit CommandGraph(bool enableGraphDiagnostics, angle::PoolAllocator *poolAllocator);
    ~CommandGraph();

    // Allocates a new CommandGraphNode and adds it to the list of current open nodes. No ordering
    // relations exist in the node by default. Call CommandGraphNode::SetHappensBeforeDependency
    // to set up dependency relations. If the node is a barrier, it will automatically add
    // dependencies between the previous barrier, the new barrier and all nodes in between.
    CommandGraphNode *allocateNode(CommandGraphNodeFunction function);

    angle::Result submitCommands(ContextVk *context,
                                 Serial serial,
                                 RenderPassCache *renderPassCache,
                                 PrimaryCommandBuffer *primaryCommandBuffer);
    bool empty() const;
    void clear();

    // The following create special-function nodes that don't require a graph resource.
    // Queries:
    void beginQuery(const QueryPool *queryPool, uint32_t queryIndex);
    void endQuery(const QueryPool *queryPool, uint32_t queryIndex);
    void writeTimestamp(const QueryPool *queryPool, uint32_t queryIndex);
    void beginTransformFeedbackEmulatedQuery();
    void endTransformFeedbackEmulatedQuery();
    // GLsync and EGLSync:
    void setFenceSync(const vk::Event &event);
    void waitFenceSync(const vk::Event &event);
    // Memory barriers:
    void memoryBarrier(VkFlags srcAccess, VkFlags dstAccess, VkPipelineStageFlags stages);
    // Debug markers:
    void insertDebugMarker(GLenum source, std::string &&marker);
    void pushDebugMarker(GLenum source, std::string &&marker);
    void popDebugMarker();
    // Host-visible buffer write availability operation:
    void makeHostVisibleBufferWriteAvailable();

    void onResourceUse(const SharedResourceUse &resourceUse);

  private:
    CommandGraphNode *allocateBarrierNode(CommandGraphNodeFunction function,
                                          CommandGraphResourceType resourceType,
                                          uintptr_t resourceID);
    void setNewBarrier(CommandGraphNode *newBarrier);
    CommandGraphNode *getLastBarrierNode(size_t *indexOut);
    void addDependenciesToNextBarrier(size_t begin, size_t end, CommandGraphNode *nextBarrier);

    void dumpGraphDotFile(std::ostream &out) const;
    void updateOverlay(ContextVk *contextVk) const;
    void releaseResourceUsesAndUpdateSerials(Serial serial);

    std::vector<CommandGraphNode *> mNodes;
    bool mEnableGraphDiagnostics;
    angle::PoolAllocator *mPoolAllocator;

    // A set of nodes (eventually) exist that act as barriers to guarantee submission order.  For
    // example, a glMemoryBarrier() calls would lead to such a barrier or beginning and ending a
    // query. This is because the graph can reorder operations if it sees fit.  Let's call a barrier
    // node Bi, and the other nodes Ni. The edges between Ni don't interest us.  Before a barrier is
    // inserted, we have:
    //
    // N0 N1 ... Na
    // \___\__/_/     (dependency egdes, which we don't care about so I'll stop drawing them.
    //      \/
    //
    // When the first barrier is inserted, we will have:
    //
    //     ______
    //    /  ____\
    //   /  /     \
    //  /  /      /\
    // N0 N1 ... Na B0
    //
    // This makes sure all N0..Na are called before B0.  From then on, B0 will be the current
    // "barrier point" which extends an edge to every next node:
    //
    //     ______
    //    /  ____\
    //   /  /     \
    //  /  /      /\
    // N0 N1 ... Na B0 Na+1 ... Nb
    //                \/       /
    //                 \______/
    //
    //
    // When the next barrier B1 is met, all nodes between B0 and B1 will add a depenency on B1 as
    // well, and the "barrier point" is updated.
    //
    //     ______
    //    /  ____\         ______         ______
    //   /  /     \       /      \       /      \
    //  /  /      /\     /       /\     /       /\
    // N0 N1 ... Na B0 Na+1 ... Nb B1 Nb+1 ... Nc B2 ...
    //                \/       /  /  \/       /  /
    //                 \______/  /    \______/  /
    //                  \_______/      \_______/
    //
    //
    // When barrier Bi is introduced, all nodes added since Bi-1 need to add a dependency to Bi
    // (including Bi-1). We therefore keep track of the node index of the last barrier that was
    // issued.
    static constexpr size_t kInvalidNodeIndex = std::numeric_limits<std::size_t>::max();
    size_t mLastBarrierIndex;

    std::vector<SharedResourceUse> mResourceUses;
};

// CommandGraphResource inlines.
ANGLE_INLINE bool CommandGraphResource::hasStartedRenderPass() const
{
    return hasChildlessWritingNode() && mCurrentWritingNode->getInsideRenderPassCommands()->valid();
}

ANGLE_INLINE void CommandGraphResource::onGraphAccess(Serial serial, CommandGraph *commandGraph)
{
    ASSERT(serial >= mUse->serial);

    // Store reference to usage in graph.
    commandGraph->onResourceUse(mUse);

    if (serial > mUse->serial)
    {
        mCurrentWritingNode = nullptr;
        mCurrentReadingNodes.clear();
        mUse->serial = serial;
    }
}

ANGLE_INLINE bool CommandGraphResource::appendToStartedRenderPass(Serial serial,
                                                                  CommandGraph *graph,
                                                                  const gl::Rectangle &renderArea,
                                                                  CommandBuffer **commandBufferOut)
{
    onGraphAccess(serial, graph);
    if (hasStartedRenderPass())
    {
        if (mCurrentWritingNode->getRenderPassRenderArea().encloses(renderArea))
        {
            *commandBufferOut = mCurrentWritingNode->getInsideRenderPassCommands();
            return true;
        }
    }

    return false;
}

ANGLE_INLINE bool CommandGraphResource::renderPassStartedButEmpty() const
{
    return hasStartedRenderPass() && (!vk::CommandBuffer::CanKnowIfEmpty() ||
                                      mCurrentWritingNode->getInsideRenderPassCommands()->empty());
}

ANGLE_INLINE void CommandGraphResource::clearRenderPassColorAttachment(
    size_t attachmentIndex,
    const VkClearColorValue &clearValue)
{
    ASSERT(renderPassStartedButEmpty());
    mCurrentWritingNode->clearRenderPassColorAttachment(attachmentIndex, clearValue);
}

ANGLE_INLINE void CommandGraphResource::clearRenderPassDepthAttachment(size_t attachmentIndex,
                                                                       float depth)
{
    ASSERT(renderPassStartedButEmpty());
    mCurrentWritingNode->clearRenderPassDepthAttachment(attachmentIndex, depth);
}

ANGLE_INLINE void CommandGraphResource::clearRenderPassStencilAttachment(size_t attachmentIndex,
                                                                         uint32_t stencil)
{
    ASSERT(renderPassStartedButEmpty());
    mCurrentWritingNode->clearRenderPassStencilAttachment(attachmentIndex, stencil);
}

ANGLE_INLINE void CommandGraphResource::invalidateRenderPassColorAttachment(size_t attachmentIndex)
{
    ASSERT(hasStartedRenderPass());
    mCurrentWritingNode->invalidateRenderPassColorAttachment(attachmentIndex);
}

ANGLE_INLINE void CommandGraphResource::invalidateRenderPassDepthAttachment(size_t attachmentIndex)
{
    ASSERT(hasStartedRenderPass());
    mCurrentWritingNode->invalidateRenderPassDepthAttachment(attachmentIndex);
}

ANGLE_INLINE void CommandGraphResource::invalidateRenderPassStencilAttachment(
    size_t attachmentIndex)
{
    ASSERT(hasStartedRenderPass());
    mCurrentWritingNode->invalidateRenderPassStencilAttachment(attachmentIndex);
}

ANGLE_INLINE const gl::Rectangle &CommandGraphResource::getRenderPassRenderArea() const
{
    ASSERT(hasStartedRenderPass());
    return mCurrentWritingNode->getRenderPassRenderArea();
}

ANGLE_INLINE void CommandGraphResource::addGlobalMemoryBarrier(VkFlags srcAccess,
                                                               VkFlags dstAccess,
                                                               VkPipelineStageFlags stages)
{
    ASSERT(mCurrentWritingNode);
    mCurrentWritingNode->addGlobalMemoryBarrier(srcAccess, dstAccess, stages);
}

ANGLE_INLINE bool CommandGraphResource::hasChildlessWritingNode() const
{
    // Note: currently, we don't have a resource that can issue both generic and special
    // commands.  We don't create read/write dependencies between mixed generic/special
    // resources either.  As such, we expect the function to always be generic here.  If such a
    // resource is added in the future, this can add a check for function == generic and fail if
    // false.
    ASSERT(mCurrentWritingNode == nullptr ||
           mCurrentWritingNode->getFunction() == CommandGraphNodeFunction::Generic);
    return (mCurrentWritingNode != nullptr && !mCurrentWritingNode->hasChildren());
}

// CommandGraph inlines.
ANGLE_INLINE void CommandGraph::onResourceUse(const SharedResourceUse &resourceUse)
{
    ASSERT(resourceUse->counter < std::numeric_limits<uint32_t>::max());
    resourceUse->counter++;
    mResourceUses.push_back(resourceUse);
}
}  // namespace vk
}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_COMMAND_GRAPH_H_
