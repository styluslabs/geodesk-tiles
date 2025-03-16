#pragma once

#include "linalg.h"

namespace geometry {

template <typename T>
using point = linalg::vec<T,2>;  //glm::vec2;

template <typename T, template <typename...> class Cont = std::vector>
struct line_string : Cont<point<T>>
{
    using coordinate_type = T;
    using point_type = point<T>;
    using container_type = Cont<point_type>;
    using container_type::container_type;
    // allow construction for plain container_type to enable conversion between line_string and linear_ring
    line_string(const container_type& lr) : container_type(lr) {}
    line_string(container_type&& lr) noexcept : container_type(std::move(lr)) {}
};

template <typename T, template <typename...> class Cont = std::vector>
struct multi_line_string : Cont<line_string<T>>
{
    using coordinate_type = T;
    using line_string_type = line_string<T>;
    using container_type = Cont<line_string_type>;
    using container_type::container_type;
};

template <typename T, template <typename...> class Cont = std::vector>
struct linear_ring : Cont<point<T>>
{
    using coordinate_type = T;
    using point_type = point<T>;
    using container_type = Cont<point_type>;
    using container_type::container_type;

    linear_ring(const container_type& ls) : container_type(ls) {}
    linear_ring(container_type&& ls) noexcept : container_type(std::move(ls)) {}
};

template <typename T, template <typename...> class Cont = std::vector>
struct polygon : Cont<linear_ring<T>>
{
    using coordinate_type = T;
    using linear_ring_type = linear_ring<T>;
    using container_type = Cont<linear_ring_type>;
    using container_type::container_type;
};

template <typename T, template <typename...> class Cont = std::vector>
struct multi_polygon : Cont<polygon<T>>
{
    using coordinate_type = T;
    using polygon_type = polygon<T>;
    using container_type = Cont<polygon_type>;
    using container_type::container_type;
};

/*
template <typename T>
struct line_string : std::vector<point<T>> {
    using container_type = std::vector<point<T>>;
    using container_type::container_type;

    line_string(const container_type& lr) : container_type(lr) {}
    line_string(container_type&& lr) noexcept : container_type(std::move(lr)) {}
    //real dist = 0.0; // line length
};

template <typename T>
struct linear_ring : std::vector<point<T>> {
    using container_type = std::vector<point<T>>;
    using container_type::container_type;

    linear_ring(const container_type& ls) : container_type(ls) {}
    linear_ring(container_type&& ls) noexcept : container_type(std::move(ls)) {}
    //real area = 0.0; // polygon ring area
};

template <typename T>
using multi_line_string = std::vector<line_string<T>>;
template <typename T>
using polygon = std::vector<linear_ring<T>>;
template <typename T>
using multi_polygon = std::vector<polygon<T>>;
*/

}
