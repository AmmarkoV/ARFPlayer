/** @file arf.hpp
 *  @brief Header-only C++ binding for libarf.
 *
 *  A thin RAII layer over <arf.h>: ownership, exceptions instead of return
 *  codes, and non-owning spans over the library's flat arrays.  It adds no
 *  copies and no state of its own -- an arf::Avatar is one pointer.
 *
 *      #include "arf.hpp"
 *
 *      arf::Avatar avatar = arf::Avatar::load("person_0.arfz");
 *      arf::Pose   pose   = avatar.pose();
 *
 *      for (unsigned int frame=0; frame<avatar.frame_count(); ++frame)
 *      {
 *          pose.evaluate(frame);
 *          draw(pose.positions(), pose.normals(), avatar.mesh_indices());
 *      }
 *
 *  Spans borrow memory owned by the avatar or the pose, so keep the owner
 *  alive while a span is in use, and re-read a pose's spans after every
 *  evaluate() -- the buffers are reused, not reallocated.
 *
 *  Requires C++11.  Link against arf (or arf_static) and miniz.
 *
 *  This reads SAM3DBody-flavoured ARF and is not a certified conformant
 *  ISO/IEC 23090-39 implementation.  See README.md.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef ARF_HPP_INCLUDED
#define ARF_HPP_INCLUDED

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

#include "arf.h"

namespace arf
{

/** @brief Thrown by every operation that libarf can fail.  The message is the
 *  library's own, which names the offending field or entry. */
class Error : public std::runtime_error
{
public:
    explicit Error(const std::string &message, int code = 0)
        : std::runtime_error(message), code_(code) {}

    /** @brief The arfResult value, or 0 when the failure was a null return. */
    int code() const noexcept { return code_; }

private:
    int code_;
};

namespace detail
{
    inline std::string last_error()
    {
        const char *message = arfLastError();
        return (message != nullptr) ? std::string(message) : std::string("unknown failure");
    }

    inline void check(int result, const std::string &what)
    {
        if (result != ARF_OK) { throw Error(what + ": " + last_error(), result); }
    }
}

/** @brief A non-owning view of a contiguous range.  std::span in all but name,
 *  spelled out here so the binding works before C++20. */
template <typename T>
class Span
{
public:
    Span() : data_(nullptr), size_(0) {}
    Span(T *data, std::size_t size) : data_(data), size_(size) {}

    T       *data()  const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool     empty() const noexcept { return size_ == 0; }

    T *begin() const noexcept { return data_; }
    T *end()   const noexcept { return data_ + size_; }

    T &operator[](std::size_t index) const { return data_[index]; }

    /** @brief Copy the range out, for when a caller wants to own it. */
    std::vector<typename std::remove_const<T>::type> to_vector() const
    {
        return std::vector<typename std::remove_const<T>::type>(begin(), end());
    }

private:
    T          *data_;
    std::size_t size_;
};

class Avatar;

/** @brief Reusable scratch buffers for one evaluated frame. */
class Pose
{
public:
    Pose(Pose &&other) noexcept : pose_(other.pose_), avatar_(other.avatar_)
    {
        other.pose_   = nullptr;
        other.avatar_ = nullptr;
    }

    Pose &operator=(Pose &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            pose_         = other.pose_;
            avatar_       = other.avatar_;
            other.pose_   = nullptr;
            other.avatar_ = nullptr;
        }
        return *this;
    }

    Pose(const Pose &) = delete;
    Pose &operator=(const Pose &) = delete;

    ~Pose() { reset(); }

    /** @brief Blendshapes, hierarchy compose, skinning and normals for one frame. */
    void evaluate(unsigned int frame)
    {
        detail::check(arfPoseEvaluate(avatar_, pose_, frame), "evaluating a frame");
    }

    unsigned int joint_count()  const noexcept { return pose_->numberOfJoints;   }
    unsigned int vertex_count() const noexcept { return pose_->numberOfVertices; }

    /** @brief Skinned vertex positions, three floats per vertex, centimetres. */
    Span<const float> positions() const
    {
        return Span<const float>(pose_->positions, std::size_t(pose_->numberOfVertices) * 3);
    }

    /** @brief Recomputed smooth vertex normals, three floats per vertex. */
    Span<const float> normals() const
    {
        return Span<const float>(pose_->normals, std::size_t(pose_->numberOfVertices) * 3);
    }

    /** @brief Model-space joint transforms, sixteen row-major floats per joint. */
    Span<const float> joint_globals() const
    {
        return Span<const float>(pose_->jointGlobals, std::size_t(pose_->numberOfJoints) * 16);
    }

    /** @brief Joint global times inverse bind, sixteen row-major floats per joint. */
    Span<const float> skin_matrices() const
    {
        return Span<const float>(pose_->skinMatrices, std::size_t(pose_->numberOfJoints) * 16);
    }

    /** @brief The underlying C pose, for mixing with the C API. */
    struct arfPose       *handle()       noexcept { return pose_; }
    const struct arfPose *handle() const noexcept { return pose_; }

private:
    friend class Avatar;

    Pose(struct arfAvatar *avatar, struct arfPose *pose) : pose_(pose), avatar_(avatar) {}

    void reset()
    {
        if (pose_ != nullptr) { arfPoseFree(pose_); pose_ = nullptr; }
    }

    struct arfPose   *pose_;
    struct arfAvatar *avatar_;
};

/** @brief An ARF container held in memory.  Move-only, like the resource it is. */
class Avatar
{
public:
    /** @brief Read a .arfz container from disk. */
    static Avatar load(const std::string &path)
    {
        struct arfAvatar *avatar = arfLoad(path.c_str());
        if (avatar == nullptr) { throw Error("cannot load \"" + path + "\": " + detail::last_error()); }
        return Avatar(avatar);
    }

    /** @brief Read a .arfz container already in memory. */
    static Avatar from_memory(const void *bytes, std::size_t length)
    {
        struct arfAvatar *avatar = arfLoadFromMemory(bytes, length);
        if (avatar == nullptr) { throw Error("cannot load the container: " + detail::last_error()); }
        return Avatar(avatar);
    }

    /** @brief Allocate an empty avatar to fill in and save. */
    static Avatar create(unsigned int nodes, unsigned int vertices,
                         unsigned int triangles, unsigned int weights)
    {
        struct arfAvatar *avatar = arfCreate(nodes, vertices, triangles, weights);
        if (avatar == nullptr) { throw Error("cannot create an avatar: " + detail::last_error()); }
        return Avatar(avatar);
    }

    Avatar(Avatar &&other) noexcept : avatar_(other.avatar_) { other.avatar_ = nullptr; }

    Avatar &operator=(Avatar &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            avatar_       = other.avatar_;
            other.avatar_ = nullptr;
        }
        return *this;
    }

    Avatar(const Avatar &) = delete;
    Avatar &operator=(const Avatar &) = delete;

    ~Avatar() { reset(); }

    // -- metadata ----------------------------------------------------------

    std::string  name()           const { return avatar_->name; }
    std::string  id()             const { return avatar_->id;   }
    int          age()            const noexcept { return avatar_->age; }
    std::string  gender()         const { return avatar_->gender; }
    unsigned int node_count()     const noexcept { return avatar_->numberOfNodes; }
    unsigned int vertex_count()   const noexcept { return avatar_->mesh.numberOfVertices; }
    unsigned int triangle_count() const noexcept { return avatar_->mesh.numberOfTriangles; }
    unsigned int frame_count()    const noexcept { return avatar_->numberOfFrames; }
    unsigned int root_node()      const noexcept { return avatar_->rootNode; }
    bool         has_face()       const noexcept { return avatar_->hasFace != 0; }
    bool         has_landmarks()  const noexcept { return avatar_->hasLandmarks != 0; }
    bool         has_texture_set() const noexcept { return avatar_->hasTextureSet != 0; }

    /** @brief Ticks per second, which for these containers is frames per second. */
    float timescale() const noexcept { return avatar_->timescale; }

    /** @brief Clip length in seconds. */
    float duration() const { return arfDuration(avatar_); }

    /** @brief Frame index nearest a wall-clock time, clamped to the clip. */
    unsigned int frame_at_time(float seconds) const { return arfFrameAtTime(avatar_, seconds); }

    /** @brief Print the library's own summary to stdout. */
    void print_info() const { arfPrintInfo(avatar_); }

    Span<const struct arfNode> nodes() const
    {
        return Span<const struct arfNode>(avatar_->nodes, avatar_->numberOfNodes);
    }

    const struct arfNode &node(unsigned int index) const
    {
        if (index >= avatar_->numberOfNodes) { throw Error("node index out of range", ARF_ERROR_ARGUMENT); }
        return avatar_->nodes[index];
    }

    // -- geometry ----------------------------------------------------------

    /** @brief Rest mesh vertices, three floats per vertex, centimetres. */
    Span<float> mesh_positions() const
    {
        return Span<float>(avatar_->mesh.positions, std::size_t(avatar_->mesh.numberOfVertices) * 3);
    }

    /** @brief Triangle vertex indices, three per triangle. */
    Span<unsigned int> mesh_indices() const
    {
        return Span<unsigned int>(avatar_->mesh.indices, std::size_t(avatar_->mesh.numberOfTriangles) * 3);
    }

    /** @brief Sixteen row-major floats per joint. */
    Span<float> inverse_bind_matrices() const
    {
        return Span<float>(avatar_->skin.inverseBindMatrices, std::size_t(avatar_->numberOfNodes) * 16);
    }

    unsigned int      weight_count()  const noexcept { return avatar_->skin.numberOfWeights; }
    Span<unsigned int> weight_vertices() const { return Span<unsigned int>(avatar_->skin.vertexIndex, weight_count()); }
    Span<unsigned int> weight_joints()   const { return Span<unsigned int>(avatar_->skin.jointIndex,  weight_count()); }
    Span<float>        weight_values()   const { return Span<float>(avatar_->skin.weight,             weight_count()); }

    // -- animation ---------------------------------------------------------

    /** @brief One frame's local joint transforms, sixteen row-major floats per
     *  joint.  Already the complete local transform -- do not also apply the
     *  node's rest translation and rotation. */
    Span<float> local_matrices(unsigned int frame) const
    {
        if (frame >= avatar_->numberOfFrames) { throw Error("frame index out of range", ARF_ERROR_ARGUMENT); }

        std::size_t stride = std::size_t(avatar_->numberOfNodes) * 16;
        return Span<float>(avatar_->localMatrices + frame * stride, stride);
    }

    Span<unsigned int> frame_timestamps() const
    {
        return Span<unsigned int>(avatar_->frameTimestamp, avatar_->numberOfFrames);
    }

    /** @brief Replace tracking-glitch frames by interpolating across them.
     *
     *  The pose estimator behind these containers regresses joint rotations as
     *  Euler angles; a joint near that representation's singularity -- the
     *  pelvis, for whole clips -- can jump branches for two or three frames and
     *  fold the body up.  The matrices stay perfectly valid, so only their
     *  velocity gives them away.  This rewrites the animation in place, which
     *  is why it is never applied for you.
     *
     *  @retval the number of frames repaired */
    unsigned int despike(float max_degrees_per_frame = ARF_DESPIKE_DEFAULT_DEGREES)
    {
        unsigned int repaired = 0;
        detail::check(arfDespikeFrames(avatar_, max_degrees_per_frame, &repaired), "despiking");
        return repaired;
    }

    /** @brief Allocate reusable per-frame buffers for this avatar. */
    Pose pose()
    {
        struct arfPose *pose = arfPoseAllocate(avatar_);
        if (pose == nullptr) { throw Error("cannot allocate a pose: " + detail::last_error()); }
        return Pose(avatar_, pose);
    }

    // -- writing -----------------------------------------------------------

    void set_node(unsigned int index, const std::string &name, int parent,
                  const float *translation = nullptr, const float *rotation = nullptr)
    {
        detail::check(arfSetNode(avatar_, index, name.c_str(), parent, translation, rotation),
                      "setting a node");
    }

    void append_frame(unsigned int timestamp, const float *local_matrices)
    {
        detail::check(arfAppendFrame(avatar_, timestamp, local_matrices), "appending a frame");
    }

    void enable_face(unsigned int shape_count, const float *deltas)
    {
        detail::check(arfEnableFace(avatar_, shape_count, deltas), "enabling the face track");
    }

    void append_face_frame(unsigned int timestamp, const float *weights)
    {
        detail::check(arfAppendFaceFrame(avatar_, timestamp, weights), "appending a face frame");
    }

    void enable_landmarks(unsigned int landmark_count, const unsigned int *vertex_index)
    {
        detail::check(arfEnableLandmarks(avatar_, landmark_count, vertex_index), "enabling the landmark track");
    }

    void append_landmark_frame(unsigned int timestamp, const float *positions)
    {
        detail::check(arfAppendLandmarkFrame(avatar_, timestamp, positions), "appending a landmark frame");
    }

    /** @brief Attach a texture set: an opaque base material image, carried
     *  but never decoded (there is no per-frame track -- TextureSet has no
     *  AAU counterpart). */
    void enable_texture_set(const std::string &name, const void *material_bytes,
                            std::size_t material_length, const std::string &material_mime_type)
    {
        detail::check(arfEnableTextureSet(avatar_, name.c_str(), material_bytes, material_length,
                                          material_mime_type.c_str()),
                      "enabling the texture set");
    }

    /** @brief Append one texture target: another opaque image blend target. */
    void add_texture_target(const std::string &name, const void *bytes,
                            std::size_t length, const std::string &mime_type)
    {
        detail::check(arfAddTextureTarget(avatar_, name.c_str(), bytes, length, mime_type.c_str()),
                      "adding a texture target");
    }

    void save(const std::string &path) const
    {
        detail::check(arfSave(avatar_, path.c_str()), "writing \"" + path + "\"");
    }

    void export_obj(const std::string &path, const float *positions = nullptr) const
    {
        detail::check(arfExportOBJ(avatar_, positions, path.c_str()), "writing \"" + path + "\"");
    }

    /** @brief The underlying C avatar, for mixing with the C API. */
    struct arfAvatar       *handle()       noexcept { return avatar_; }
    const struct arfAvatar *handle() const noexcept { return avatar_; }

private:
    explicit Avatar(struct arfAvatar *avatar) : avatar_(avatar) {}

    void reset()
    {
        if (avatar_ != nullptr) { arfFree(avatar_); avatar_ = nullptr; }
    }

    struct arfAvatar *avatar_;
};

}  // namespace arf

#endif  // ARF_HPP_INCLUDED
