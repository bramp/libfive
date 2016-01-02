#include <limits>

#include "ao/render/heightmap.hpp"
#include "ao/core/tree.hpp"
#include "ao/gl/accelerator.hpp"

#include <iostream>

namespace Heightmap
{

/*
 *  Helper functions that evaluates a region of pixels
 */
static void pixels(Tree* t, const Region& r, DepthImage& depth, NormalImage& norm)
{
    const double* out = t->eval(r);

    int index = 0;

    // Store the x, y coordinates of rendered points for normal calculations
    constexpr size_t NUM_POINTS = Result::count<Gradient>();
    size_t xs[NUM_POINTS];
    size_t ys[NUM_POINTS];
    size_t norm_count = 0;

    // Helper function to calculate a set of gradients and apply them to
    // the normal image
    auto run = [&](){
        const Gradient* gs = t->evalCore<Gradient>(norm_count);
        for (size_t i=0; i < norm_count; ++i)
        {
            // Find the normal's length (to normalize it)
            double len = sqrt(pow(gs[i].dx, 2) +
                              pow(gs[i].dy, 2) +
                              pow(gs[i].dz, 2));

            // Pack each normal into the 0-255 range
            uint32_t dx = 255 * (gs[i].dx / (2 * len) + 0.5);
            uint32_t dy = 255 * (gs[i].dy / (2 * len) + 0.5);
            uint32_t dz = 255 * (gs[i].dz / (2 * len) + 0.5);

            // Pack the normals and a dummy alpha byte into the image
            norm(ys[i], xs[i]) = (0xff << 24) | (dz << 16) | (dy << 8) | dx;
        }
        norm_count = 0;
    };

    // Unflatten results into the image, breaking out of loops early when a pixel
    // is written (because all subsequent pixels will be below it).  This
    // loop's behavior is dependent on how Tree::eval(Region) is structured.
    REGION_ITERATE_XYZ(r)
    {
        // If this voxel is filled (because the f-rep is less than zero)
        if (out[index++] < 0)
        {
            // Check to see whether the voxel is in front of the image's depth
            const double z = r.Z.pos(r.Z.size - k - 1);
            if (depth(r.Y.min + j, r.X.min + i) < z)
            {
                depth(r.Y.min + j, r.X.min + i) = z;

                // Adjust the index pointer, since we can skip the rest of
                // this z-column (since future voxels are behind this one)
                index += r.Z.size - k - 1;

                // Store information for rendering gradients in bulk
                xs[norm_count] = r.X.min + i;
                ys[norm_count] = r.Y.min + j;
                t->setPoint<Gradient>(Gradient(r.X.pos(i), 1, 0, 0),
                                      Gradient(r.Y.pos(j), 0, 1, 0),
                                      Gradient(r.Z.pos(k), 0, 0, 1),
                                      norm_count++);
                // If the gradient array is completely full, execute a
                // calculation that finds normals and blits them to the image
                if (norm_count == NUM_POINTS)
                {
                    run();
                }

                break;
            }
        }
    }

    // Render the last of the normal calculations
    if (norm_count > 0)
    {
        run();
    }
}

/*
 * Helper function that reduces a particular matrix block
 */
static void recurse(Tree* t, const Region& r, DepthImage& depth,
                    NormalImage& norm,const std::atomic<bool>& abort)
{
    // Stop rendering if the abort flag is set
    if (abort.load())
    {
        return;
    }

    // Extract the block of the image that's being inspected
    auto block = depth.block(r.Y.min, r.X.min, r.Y.size, r.X.size);

    // If all points in the region are below the heightmap, skip it
    if ((block >= r.Z.pos(r.Z.size - 1)).all())
    {
        return;
    }

    // If we're below a certain size, render pixel-by-pixel
    if (r.voxels() <= Result::count<double>())
    {
        pixels(t, r, depth, norm);
        return;
    }

    // Do the interval evaluation
    Interval out = t->eval(r.X.interval, r.Y.interval, r.Z.interval);

    // If strictly negative, fill up the block and return
    if (out.upper() < 0)
    {
        block = block.max(r.Z.pos(r.Z.size - 1));
    }
    // Otherwise, recurse if the output interval is ambiguous
    else if (out.lower() <= 0)
    {
        // Disable inactive nodes in the tree
        t->push();

        // Subdivide and recurse
        assert(r.canSplit());

        auto rs = r.split();

        // Since the higher Z region is in the second item of the
        // split, evaluate rs.second then rs.first
        recurse(t, rs.second, depth, norm, abort);
        recurse(t, rs.first, depth, norm, abort);

        // Re-enable disabled nodes from the tree
        t->pop();
    }
}

std::pair<DepthImage, NormalImage> Render(
        Tree* t, Region r, const std::atomic<bool>& abort, bool clip)
{
    auto depth = DepthImage(r.Y.size, r.X.size);
    auto norm = NormalImage(r.Y.size, r.X.size);

    depth.fill(-std::numeric_limits<double>::infinity());
    norm.fill(0);

#if 0
    if (auto accel = t->getAccelerator())
    {
        accel->makeContextCurrent();
        accel->Render(r, depth);
    }
    else
#endif
    {
        recurse(t, r, depth, norm, abort);
    }

    // If the pixel is touching the top Z boundary and clip is true,
    // set this pixel's normal to be pointing in the Z direction
    if (clip)
    {
        norm = (depth == r.Z.pos(r.Z.size - 1)).select(0xffff7f7f, norm);
    }

    return std::make_pair(depth, norm);
}


} // namespace Heightmap
