/*

This file is part of VROOM.

Copyright (c) 2015-2018, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include "../include/rapidjson/document.h"
#include "../include/rapidjson/error/en.h"
#include <boost/asio.hpp>

#include "routing/routed_wrapper.h"
#include "utils/exception.h"

using boost::asio::ip::tcp;

namespace vroom {
namespace routing {

RoutedWrapper::RoutedWrapper(const std::string& profile, const Server& server)
  : OSRMWrapper(profile), _server(server) {
}

std::string RoutedWrapper::build_query(const std::vector<Location>& locations,
                                       std::string service,
                                       std::string extra_args = "") const {
  // Building query for osrm-routed
  std::string query = "GET /" + service;

  query += "/v1/" + _profile + "/";

  // Adding locations.
  for (auto const& location : locations) {
    query += std::to_string(location.lon()) + "," +
             std::to_string(location.lat()) + ";";
  }
  query.pop_back(); // Remove trailing ';'.

  if (!extra_args.empty()) {
    query += "?" + extra_args;
  }

  query += " HTTP/1.1\r\n";
  query += "Host: " + _server.host + "\r\n";
  query += "Accept: */*\r\n";
  query += "Connection: close\r\n\r\n";

  return query;
}

std::string RoutedWrapper::send_then_receive(std::string query) const {
  std::string response;

  try {
    boost::asio::io_service io_service;

    tcp::resolver r(io_service);

    tcp::resolver::query q(_server.host, _server.port);

    tcp::socket s(io_service);
    boost::asio::connect(s, r.resolve(q));

    boost::asio::write(s, boost::asio::buffer(query));

    char buf[512];
    boost::system::error_code error;
    for (;;) {
      std::size_t len = s.read_some(boost::asio::buffer(buf), error);
      response.append(buf, len);
      if (error == boost::asio::error::eof) {
        // Connection closed cleanly.
        break;
      } else {
        if (error) {
          throw boost::system::system_error(error);
        }
      }
    }
  } catch (boost::system::system_error& e) {
    throw Exception(ERROR::ROUTING,
                    "Failed to connect to OSRM at " + _server.host + ":" +
                      _server.port);
  }
  return response;
}

Matrix<Cost>
RoutedWrapper::get_matrix(const std::vector<Location>& locs) const {
  std::string query = this->build_query(locs, "table");

  std::string response = this->send_then_receive(query);

  // Removing headers.
  std::string json_content = response.substr(response.find("{"));

  // Expected matrix size.
  std::size_t m_size = locs.size();

  // Checking everything is fine in the response.
  rapidjson::Document infos;
#ifdef NDEBUG
  infos.Parse(json_content.c_str());
#else
  assert(!infos.Parse(json_content.c_str()).HasParseError());
  assert(infos.HasMember("code"));
#endif
  if (infos["code"] != "Ok") {
    throw Exception(ERROR::ROUTING,
                    "OSRM table: " + std::string(infos["message"].GetString()));
  }
  assert(infos["durations"].Size() == m_size);

  // Build matrix while checking for unfound routes to avoid
  // unexpected behavior (OSRM raises 'null').
  Matrix<Cost> m(m_size);

  std::vector<unsigned> nb_unfound_from_loc(m_size, 0);
  std::vector<unsigned> nb_unfound_to_loc(m_size, 0);

  for (rapidjson::SizeType i = 0; i < infos["durations"].Size(); ++i) {
    const auto& line = infos["durations"][i];
    assert(line.Size() == m_size);
    for (rapidjson::SizeType j = 0; j < line.Size(); ++j) {
      if (line[j].IsNull()) {
        // No route found between i and j. Just storing info as we
        // don't know yet which location is responsible between i
        // and j.
        ++nb_unfound_from_loc[i];
        ++nb_unfound_to_loc[j];
      } else {
        auto cost = round_cost(line[j].GetDouble());
        //TODO c'est ici qu'il faut rajouter le pourcentage pour ca je doit le récuperer en argument peut être au setup ?
        m[i][j] = cost + (malus * cost / 100);
      }
    }
  }

  check_unfound(locs, nb_unfound_from_loc, nb_unfound_to_loc);

  return m;
}

void RoutedWrapper::add_route_info(Route& route) const {
  // Ordering locations for the given steps.
  std::vector<Location> ordered_locations;
  for (const auto& step : route.steps) {
    ordered_locations.push_back(step.location);
  }

  std::string extra_args =
    "alternatives=false&steps=false&overview=full&continue_straight=false";

  std::string query = this->build_query(ordered_locations, "route", extra_args);
  std::string response = this->send_then_receive(query);

  // Removing headers
  std::string json_content = response.substr(response.find("{"));

  // Checking everything is fine in the response.
  rapidjson::Document infos;
#ifdef NDEBUG
  infos.Parse(json_content.c_str());
#else
  assert(!infos.Parse(json_content.c_str()).HasParseError());
  assert(infos.HasMember("code"));
#endif
  if (infos["code"] != "Ok") {
    throw Exception(ERROR::ROUTING,
                    "OSRM route: " + std::string(infos["message"].GetString()));
  }

  // Total distance and route geometry.
  route.distance = round_cost(infos["routes"][0]["distance"].GetDouble());
  route.geometry = std::move(infos["routes"][0]["geometry"].GetString());

  auto nb_legs = infos["routes"][0]["legs"].Size();
  assert(nb_legs == route.steps.size() - 1);

  // Accumulated travel distance stored for each step.
  double current_distance = 0;

  route.steps[0].distance = 0;

  for (rapidjson::SizeType i = 0; i < nb_legs; ++i) {
    // Update distance for step after current route leg.
    current_distance += infos["routes"][0]["legs"][i]["distance"].GetDouble();
    route.steps[i + 1].distance = round_cost(current_distance);
  }
}

} // namespace routing
} // namespace vroom
