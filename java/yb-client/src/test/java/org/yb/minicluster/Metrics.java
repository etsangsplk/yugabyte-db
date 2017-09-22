// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

package org.yb.minicluster;

import java.io.IOException;
import java.net.MalformedURLException;
import java.net.URL;
import java.util.Map;
import java.util.HashMap;
import java.util.Scanner;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

/**
 * A class to retrieve metrics from a YB server.
 */
public class Metrics {

  /**
   * The base metric.
   */
  public static class Metric {
    public final String name;

    /**
     * Constructs a base {@code Metric}.
     *
     * @param metric  the JSON object that contains the metric
     */
    protected Metric(JsonObject metric) {
      name = metric.get("name").getAsString();
    }
  }

  /**
   * A counter metric.
   */
  public static class Counter extends Metric {
    public final int value;

    /**
     * Constructs a {@code Counter} metric.
     *
     * @param metric  the JSON object that contains the metric
     */
    Counter(JsonObject metric) {
      super(metric);
      value = metric.get("value").getAsInt();
    }
  }

  /**
   * A histogram metric.
   */
  public static class Histogram extends Metric {
    public int totalCount;
    public int min;
    public int mean;
    public int median;
    public int std_dev;
    public int percentile75;
    public int percentile95;
    public int percentile99;
    public int percentile999;
    public int percentile9999;
    public int max;
    public int totalSum;

    /**
     * Constructs a {@code Histogram} metric.
     *
     * @param metric  the JSON object that contains the metric
     */
    Histogram(JsonObject metric) {
      super(metric);
      for (Map.Entry<String, JsonElement> elem : metric.entrySet()) {
        String name = elem.getKey();
        if (name.equals("name"))
          continue;
        int value = elem.getValue().getAsInt();
        switch (name) {
          case "total_count": totalCount = value; break;
          case "min": min = value; break;
          case "mean": mean = value; break;
          case "median": median = value; break;
          case "std_dev": std_dev = value; break;
          case "percentile_75": percentile75 = value; break;
          case "percentile_95": percentile95 = value; break;
          case "percentile_99": percentile99 = value; break;
          case "percentile_99_9": percentile999 = value; break;
          case "percentile_99_99": percentile9999 = value; break;
          case "max": max = value; break;
          case "total_sum": totalSum = value; break;
        }
      }
    }
  }

  // The metrics map.
  Map<String, Metric> map;

  /**
   * Constructs a {@code Metrics} to retrieve the metrics.
   *
   * @param obj   the metric in JSON
   */
  public Metrics(JsonObject obj) {
    readMetrics(obj);
  }

  /**
   * Constructs a {@code Metrics} to retrieve the metrics.
   *
   * @param host  the host where the metrics web server is listening
   * @param port  the port where the metrics web server is listening
   * @param type  the metrics type
   */
  public Metrics(String host, int port, String type) throws IOException {
    try {
      URL url = new URL(String.format("http://%s:%d/metrics", host, port));
      Scanner scanner = new Scanner(url.openConnection().getInputStream());
      JsonParser parser = new JsonParser();
      JsonElement tree = parser.parse(scanner.useDelimiter("\\A").next());
      for (JsonElement elem : tree.getAsJsonArray()) {
        JsonObject obj = elem.getAsJsonObject();
        if (obj.get("type").getAsString().equals(type)) {
          readMetrics(obj);
          break;
        }
      }
    } catch (MalformedURLException e) {
      throw new InternalError(e.getMessage());
    }
  }

  // Read metrics.
  private void readMetrics(JsonObject obj) {
    map = new HashMap<>();
    for (JsonElement subelem : obj.getAsJsonArray("metrics")) {
      JsonObject metric = subelem.getAsJsonObject();
      if (metric.has("value")) {
        Counter counter = new Counter(metric);
        map.put(counter.name, counter);
      } else if (metric.has("total_count")) {
        Histogram histogram = new Histogram(metric);
        map.put(histogram.name, histogram);
      }
    }
  }

  /**
   * Retrieves a {@code Counter} metric.
   *
   * @param name  the metric name
   */
  public Counter getCounter(String name) {
    return (Counter)map.get(name);
  }

  /**
   * Retrieves a {@code Histogram} metric.
   *
   * @param name  the metric name
   */
  public Histogram getHistogram(String name) {
    return (Histogram)map.get(name);
  }
}
