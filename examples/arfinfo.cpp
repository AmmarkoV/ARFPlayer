/** @file arfinfo.cpp
 *  @brief Example use of the C++ binding: summarise a container, measure a
 *         frame, and optionally re-encode it.
 *
 *  Build:  cmake -S . -B build && cmake --build build
 *  Run:    ./build/arfinfo_cpp samples/summerlove_0.arfz
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf.hpp"

#include <cstdio>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: arfinfo_cpp <container.arfz> [output.arfz]\n";
        return 1;
    }

    try
    {
        arf::Avatar avatar = arf::Avatar::load(argv[1]);

        std::cout << avatar.name() << " (" << avatar.id() << ")\n"
                  << "  " << avatar.node_count()     << " joints, root \""
                           << avatar.node(avatar.root_node()).id << "\"\n"
                  << "  " << avatar.vertex_count()   << " vertices, "
                           << avatar.triangle_count() << " triangles, "
                           << avatar.weight_count()   << " skin weights\n"
                  << "  " << avatar.frame_count()    << " frames at "
                           << avatar.timescale() << " fps, "
                           << avatar.duration() << " s\n";

        // Containers carry raw tracking output, including the occasional
        // glitch burst; repairing is opt-in, so ask for it explicitly.
        unsigned int repaired = avatar.despike();
        if (repaired > 0) { std::cout << "  repaired " << repaired << " glitch frame(s)\n"; }

        // Pose the middle frame and report where the mesh ended up, which is
        // the cheapest end to end check that skinning ran.
        arf::Pose pose = avatar.pose();
        unsigned int middle = avatar.frame_count() / 2;
        pose.evaluate(middle);

        float minimum[3] = {  1e30f,  1e30f,  1e30f };
        float maximum[3] = { -1e30f, -1e30f, -1e30f };

        arf::Span<const float> positions = pose.positions();
        for (std::size_t i=0; i<positions.size(); i+=3)
        {
            for (unsigned int c=0; c<3; c++)
            {
                if (positions[i+c] < minimum[c]) { minimum[c] = positions[i+c]; }
                if (positions[i+c] > maximum[c]) { maximum[c] = positions[i+c]; }
            }
        }

        std::printf("  frame %u posed bbox X=[%.1f,%.1f] Y=[%.1f,%.1f] Z=[%.1f,%.1f] cm\n",
                    middle,minimum[0],maximum[0],minimum[1],maximum[1],minimum[2],maximum[2]);

        if (argc >= 3)
        {
            avatar.save(argv[2]);
            std::cout << "  re-encoded to " << argv[2] << "\n";
        }
    }
    catch (const arf::Error &problem)
    {
        std::cerr << "arf error: " << problem.what() << "\n";
        return 1;
    }

    return 0;
}
